// SPDX-License-Identifier: GPL-2.0
/*
 * test_alloc.c - the cluster allocator, ntfs_cluster_alloc() in
 * core/ntfs/lcnalloc.c.
 *
 * Linux 7.1's allocator, which this port carries verbatim, has two defects
 * that mainline fixed in October 2026. Both are backported here
 * (docs/UPSTREAM-BUGS.md U13 and U14):
 *
 *   18d4d2a3ce75, "ntfs: restart the zone search when the allocation hint
 *   fails". When the clusters at the caller's hint do not satisfy a request,
 *   the search moves to the zone position but keeps its pass, its zone end and
 *   its has_guess state. It then tries the single cluster at the zone
 *   position, skips the rest of the bitmap page when that cluster is in use,
 *   shrinks the MFT zone to nothing and fails with -ENOSPC while clusters are
 *   free. A hint at or past the end of the volume is enough to get there, and
 *   an attribute whose last run ends on the last cluster hands exactly that
 *   hint to its next extension.
 *
 *   b05a67615b98, "ntfs: do not give a contiguous cluster allocation a second
 *   run". A contiguous request whose run reaches the end of a bitmap page or
 *   of a zone carries on wherever the scan goes next (the zone position,
 *   pass 2, a shrunk MFT zone), and the first free cluster there starts a
 *   second run. A caller that asked for contiguous clusters uses the first run
 *   alone.
 *
 * The first is visible through the ABI, and test_names_wall finds it the way
 * it was found: by filling a small root folder with names. The second is not
 * visible in this port. Its contiguous callers are ntfs_attr_map_cluster(),
 * whose callers here zero or write every run it maps, and the compressed
 * writer, which a short run breaks as badly as a second one (U14). So
 * test_alloc_cases calls the allocator itself, on a $Bitmap crafted on the
 * image before the mount, with a case for each way of reaching each defect.
 * test_fallocate_hole_zeroes keeps upstream's symptom as a guard.
 *
 * A third fix, from August 2026, is backported as well (U15):
 *
 *   19cac7902a8a, "ntfs: bound the free-cluster bitmap scan to the volume".
 *   The allocator bounded its scan by $Bitmap's size rather than the volume's,
 *   and the run-list decoder accepted an LCN past the end of the volume.
 *   test_run_past_volume_end and test_bitmap_longer_than_volume reach each
 *   half of it through a crafted image.
 *
 * Two more from that month follow (U16, U17), both in error paths that free
 * clusters. be9e89ccb8e5 is reached by test_free_with_unreadable_bitmap, with
 * $Bitmap made unreadable. 5f2a22b36fe3 has no test: U17 says why its path
 * cannot be reached on demand.
 *
 * Every case ends with ntfsck -n on the image, except the crafted images of
 * section 5, which are corrupt by construction.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <sys/mman.h>

/*
 * White-box: the allocator is called directly, so this file sees the driver's
 * own headers. core/ntfs/inode.h declares an internal struct ntfs_attr and
 * ntfs_getattr()/ntfs_setattr(), which ntfscore.h also names; rename the
 * internal ones, as core/vfs/api.c does.
 */
#define ntfs_attr ntfs_attr_internal
#define ntfs_getattr ntfs_getattr_internal
#define ntfs_setattr ntfs_setattr_internal
#include "ntfs.h"
#include "lcnalloc.h"
#undef ntfs_attr
#undef ntfs_getattr
#undef ntfs_setattr
#include "glue.h"		/* struct ntfs_volume_handle, and ntfscore.h */
#include "ntfsport/bdev.h"

static int failures, checks;
#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

#define CHECK_MSG(cond, ...) do {					\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
	}								\
} while (0)

/* ---- images ---------------------------------------------------------- */

static char img[1024];

static const char *tool(const char *envvar, const char *fallback)
{
	const char *v = getenv(envvar);
	return v && *v ? v : fallback;
}

/* A fresh mkntfs volume of @mb megabytes, made the way test_faults.c makes them. */
static int make_image(const char *tag, unsigned mb)
{
	char cmd[2048];

	snprintf(img, sizeof(img), "/tmp/ttntfs-alloc-%d-%s.img", (int)getpid(), tag);
	snprintf(cmd, sizeof(cmd),
		 "dd if=/dev/zero of='%s' bs=1m count=%u >/dev/null 2>&1 && "
		 "'%s' -Q -F -L ALLOC '%s' >/dev/null 2>&1",
		 img, mb, tool("NTFS_MKNTFS", "tools/.local/sbin/mkntfs"), img);
	if (system(cmd) != 0) {
		fprintf(stderr, "SKIP %s: could not build an image (mkntfs missing?)\n", tag);
		img[0] = 0;
		return -1;
	}
	return 0;
}

/* ntfsck -n, exit 0 required. Never without -n: it would repair the image. */
static void check_image(const char *tag)
{
	char cmd[2048], out[1024];
	int rc;

	snprintf(out, sizeof(out), "/tmp/ttntfs-alloc-%d-fsck.txt", (int)getpid());
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' >'%s' 2>&1",
		 tool("NTFS_NTFSCK", "tools/.local-plus/sbin/ntfsck"), img, out);
	rc = system(cmd);
	checks++;
	if (rc != 0) {
		char line[512];
		FILE *f = fopen(out, "r");

		failures++;
		fprintf(stderr, "FAIL %s: ntfsck -n exited %d\n", tag, rc);
		if (f) {
			while (fgets(line, sizeof(line), f))
				fprintf(stderr, "    | %s", line);
			fclose(f);
		}
	}
	unlink(out);
}

static void drop_image(void)
{
	if (img[0])
		unlink(img);
	img[0] = 0;
}

static const struct ntfs_mount_options RW_OPTS = {
	.flags = 0, .uid = 0, .gid = 0, .fmask = 022, .dmask = 022
};

static int mount_rw(struct ntfs_bdev **dev, ntfs_volume_t **vol)
{
	int err;

	*dev = ntfs_bdev_open_path(img, false);
	if (!*dev)
		return -ENOENT;
	err = ntfs_mount(*dev, &RW_OPTS, vol);
	if (err)
		ntfs_bdev_close(*dev);
	return err;
}

static void umount_vol(struct ntfs_bdev *dev, ntfs_volume_t *vol)
{
	ntfs_volume_sync(vol);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
}

/* ---- the image, read and written raw while it is unmounted ------------ */

static struct {
	uint32_t cluster_size;
	uint32_t rec_size;	/* MFT record size */
	uint64_t nr_clusters;
	uint64_t mft_lcn;
	uint64_t off;		/* byte offset of $Bitmap's data on the image */
	size_t len;		/* its data size */
} bmp;

static uint64_t le_at(const uint8_t *p, int n)
{
	uint64_t v = 0;

	while (n--)
		v = v << 8 | p[n];
	return v;
}

static void put_le(uint8_t *p, uint64_t v, int n)
{
	while (n--) {
		*p++ = (uint8_t)v;
		v >>= 8;
	}
}

static int image_io(void *buf, size_t len, uint64_t at, bool write)
{
	int fd = open(img, write ? O_RDWR : O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write ? pwrite(fd, buf, len, (off_t)at) : pread(fd, buf, len, (off_t)at);
	close(fd);
	return n == (ssize_t)len ? 0 : -1;
}

/* Boot sector geometry. */
static int read_boot(void)
{
	uint8_t bs[512];
	int8_t cpr;

	if (image_io(bs, sizeof(bs), 0, false))
		return -1;
	bmp.cluster_size = (uint32_t)le_at(bs + 0x0b, 2) * bs[0x0d];
	bmp.nr_clusters = le_at(bs + 0x28, 8) * le_at(bs + 0x0b, 2) / bmp.cluster_size;
	bmp.mft_lcn = le_at(bs + 0x30, 8);
	cpr = (int8_t)bs[0x40];
	bmp.rec_size = cpr > 0 ? (uint32_t)cpr * bmp.cluster_size : 1u << -cpr;
	return bmp.rec_size > 4096 ? -1 : 0;
}

/* The offset of the unnamed attribute of @type in a fixed-up record, or 0. */
static uint32_t find_attr(const uint8_t *rec, uint32_t type)
{
	uint32_t off, len;

	for (off = (uint32_t)le_at(rec + 0x14, 2); off + 0x18 <= bmp.rec_size; off += len) {
		uint32_t t = (uint32_t)le_at(rec + off, 4);

		len = (uint32_t)le_at(rec + off + 4, 4);
		if (t == 0xffffffff || len < 0x18 || off + len > bmp.rec_size)
			break;
		if (t == type && !rec[off + 9])
			return off;
	}
	return 0;
}

/*
 * The LCN behind @vcn in the mapping pairs at @mp, or -1. Signed deltas, as
 * NTFS writes them; holes have no LCN bytes.
 */
static int64_t map_vcn(const uint8_t *mp, const uint8_t *end, uint64_t vcn)
{
	uint64_t at = 0;
	int64_t lcn = 0;

	while (mp < end && *mp) {
		int nl = *mp & 15, no = *mp >> 4;
		uint64_t len;

		if (!nl || nl > 8 || no > 8 || mp + 1 + nl + no > end)
			return -1;
		len = le_at(mp + 1, nl);
		if (no) {
			int64_t d = (int64_t)le_at(mp + 1 + nl, no);

			if (no < 8 && mp[nl + no] & 0x80)
				d -= (int64_t)1 << (8 * no);
			lcn += d;
		}
		if (vcn < at + len)
			return no ? lcn + (int64_t)(vcn - at) : -1;
		at += len;
		mp += 1 + nl + no;
	}
	return -1;
}

static void undo_fixups(uint8_t *rec)
{
	uint32_t usa = (uint32_t)le_at(rec + 4, 2), n = (uint32_t)le_at(rec + 6, 2);

	for (uint32_t i = 1; i < n && i * 512 <= bmp.rec_size; i++)
		memcpy(rec + i * 512 - 2, rec + usa + 2 * i, 2);
}

/*
 * MFT record @no with its update sequence fixups undone. Returns the byte
 * offset it was read from, which write_record() takes, or -1. The record is
 * found through $MFT's own run list in record 0.
 */
static int64_t read_record(uint64_t no, uint8_t *rec)
{
	uint64_t byte = no * bmp.rec_size;
	uint32_t a;
	int64_t lcn, at;

	if (image_io(rec, bmp.rec_size, bmp.mft_lcn * bmp.cluster_size, false) ||
	    memcmp(rec, "FILE", 4))
		return -1;
	undo_fixups(rec);
	a = find_attr(rec, 0x80);
	if (!a || !rec[a + 8])
		return -1;
	lcn = map_vcn(rec + a + le_at(rec + a + 0x20, 2), rec + a + le_at(rec + a + 4, 4),
		      byte / bmp.cluster_size);
	if (lcn < 0)
		return -1;
	at = lcn * (int64_t)bmp.cluster_size + (int64_t)(byte % bmp.cluster_size);
	if (image_io(rec, bmp.rec_size, (uint64_t)at, false) || memcmp(rec, "FILE", 4))
		return -1;
	undo_fixups(rec);
	return at;
}

/* Redo the fixups of a record read by read_record() and write it back. */
static int write_record(int64_t at, uint8_t *rec)
{
	uint32_t usa = (uint32_t)le_at(rec + 4, 2), n = (uint32_t)le_at(rec + 6, 2);

	for (uint32_t i = 1; i < n && i * 512 <= bmp.rec_size; i++) {
		memcpy(rec + usa + 2 * i, rec + i * 512 - 2, 2);
		memcpy(rec + i * 512 - 2, rec + usa, 2);
	}
	return image_io(rec, bmp.rec_size, (uint64_t)at, true);
}

/*
 * Find $Bitmap's data: MFT record 6, its unnamed $DATA. The volumes here are
 * small enough that it is one run; anything else is reported rather than
 * guessed at.
 */
static int locate_bitmap(void)
{
	uint8_t rec[4096];
	const uint8_t *mp;
	uint64_t len;
	uint32_t a;
	int nl, no;

	if (read_boot() || read_record(6, rec) < 0)
		return -1;
	a = find_attr(rec, 0x80);
	if (!a || !rec[a + 8])
		return -1;
	mp = rec + a + le_at(rec + a + 0x20, 2);
	nl = mp[0] & 15;
	no = mp[0] >> 4;
	len = le_at(mp + 1, nl);
	bmp.off = le_at(mp + 1 + nl, no) * bmp.cluster_size;
	bmp.len = (size_t)le_at(rec + a + 0x30, 8);
	if (mp[1 + nl + no] || len * bmp.cluster_size < bmp.len)
		return -1;
	return 0;
}

static int bitmap_io(uint8_t *buf, bool write)
{
	return image_io(buf, bmp.len, bmp.off, write);
}

static bool bit_set(const uint8_t *b, uint64_t lcn)
{
	return b[lcn >> 3] >> (lcn & 7) & 1;
}

static void mark_used(uint8_t *b, uint64_t from, uint64_t to)
{
	for (uint64_t i = from; i < to; i++)
		b[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/*
 * Clear [from, to) in @b, but only where mkntfs left it free: a case must never
 * hand out a cluster that holds a system file. If the fixture's layout moves,
 * say so instead.
 */
static bool mark_free(uint8_t *b, const uint8_t *orig, uint64_t from, uint64_t to)
{
	for (uint64_t i = from; i < to; i++) {
		if (bit_set(orig, i)) {
			CHECK_MSG(false, "cluster %" PRIu64 " is in use on a fresh image: the "
				  "layout these cases assume has moved", i);
			return false;
		}
		b[i >> 3] &= (uint8_t)~(1u << (i & 7));
	}
	return true;
}

/* ====================================================================== */
/* 1. the defect as it was found: a full root folder, clusters still free */
/* ====================================================================== */

/*
 * 18d4d2a3ce75's -ENOSPC through the ABI. A 2 MB volume's root is filled with
 * empty 8-character names, which take MFT records and index blocks but no
 * data. The root's $INDEX_ALLOCATION grows a cluster at a time with the hint
 * "after my last run", and the fill comes to a point where that run ends on
 * the volume's last cluster.
 *
 * Measured on the pinned mkntfs, 511 clusters: 0.4.2 stopped after 846 names
 * with 70 clusters free, as EPERM because ntfs_enlarge_attribute() drops the
 * -ENOSPC (U10). With the fix, after 1076 names with one cluster free. The
 * errno is not this test's business; the clusters left free are.
 *
 * That one cluster is allowed, and it is always the same one: LCN 3, the gap
 * mkntfs leaves in front of $MFT, inside the MFT zone. The request that fails
 * there is $MFT's own extension (which, until U11 is fixed, also sets the
 * volume read-only). Its hint lies past the MFT zone, so the search skips that
 * zone and never reaches LCN 3 (done_zones |= 1). That happens with and
 * without either fix.
 */
static void test_names_wall(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	struct ntfs_volume_info info;
	char name[32];
	int err = 0, i, created = 0;

	if (make_image("names", 2) != 0)
		return;
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) {
		drop_image();
		return;
	}
	CHECK(ntfs_volume_root(vol, &root) == 0);
	for (i = 0; i < 20000; i++) {
		snprintf(name, sizeof(name), "e%05dxx", i);
		err = ntfs_create(root, name, 0100644, &f);
		if (err)
			break;
		ntfs_inode_put(f);
		created++;
	}
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	printf("  names: %d created, then %d (%s) with %" PRIu64 " of %" PRIu64
	       " clusters free\n", created, err, strerror(-err),
	       info.free_clusters, info.total_clusters);
	CHECK_MSG(err != 0, "20000 names fitted in a 2 MB volume");
	CHECK_MSG(info.free_clusters <= 1,
		  "the volume stopped taking names with %" PRIu64 " clusters free "
		  "(%d names, error %d): the allocator gave up on them",
		  info.free_clusters, created, err);
	ntfs_inode_put(root);
	umount_vol(dev, vol);
	check_image("names");
	drop_image();
	printf("test_names_wall\n");
}

/* ====================================================================== */
/* 2. the allocator itself, on a crafted $Bitmap                          */
/* ====================================================================== */

#define MAX_RUNS 8

struct outcome {
	int err;			/* 0, or what the request returned */
	int nruns;
	struct { s64 vcn, lcn, len; } run[MAX_RUNS];
	s64 free_seen;			/* free clusters when the request was made */
	s64 prime_lcn, prime_len;	/* where the priming allocation went */
	bool prime_held;		/* ...and it was still allocated during the request */
	bool bitmap_back;		/* $Bitmap after unmount == the crafted one */
};

/*
 * Mount with @crafted as the volume's $Bitmap. Optionally allocate @prime
 * clusters with no hint first, which moves the data zone position the way any
 * earlier write would; @prime_keep holds them until after the request. Then
 * make the request the way ntfs_attr_map_cluster() (contiguous) or
 * ntfs_non_resident_attr_expand() (not) makes it, record what came back, free
 * everything, unmount, and compare the bitmap on disk with @crafted: every
 * cluster handed out must have been given back.
 */
static void request(const uint8_t *crafted, s64 prime, bool prime_keep,
		    s64 count, s64 start_lcn, bool contig, struct outcome *o)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *h;
	struct runlist_element *rl, *pr = NULL;
	uint8_t *after = malloc(bmp.len);
	int err;

	memset(o, 0, sizeof(*o));
	o->err = -EINVAL;
	if (!after || bitmap_io((uint8_t *)crafted, true)) {
		free(after);
		return;
	}
	err = mount_rw(&dev, &h);
	CHECK_MSG(err == 0, "mount over a crafted $Bitmap: %d", err);
	if (err) {
		free(after);
		return;
	}
	if (prime) {
		pr = ntfs_cluster_alloc(h->vol, 0, prime, -1, DATA_ZONE, false, false, false);
		CHECK_MSG(!IS_ERR(pr), "priming allocation of %lld: %ld", (long long)prime,
			  IS_ERR(pr) ? PTR_ERR(pr) : 0L);
		if (IS_ERR(pr)) {
			pr = NULL;
		} else {
			o->prime_lcn = pr->lcn;
			o->prime_len = pr->length;
			o->prime_held = prime_keep;
			if (!prime_keep) {
				CHECK(ntfs_cluster_free_from_rl(h->vol, pr) == 0);
				kvfree(pr);
				pr = NULL;
			}
		}
	}
	o->free_seen = atomic64_read(&h->vol->free_clusters);
	rl = ntfs_cluster_alloc(h->vol, 0, count, start_lcn, DATA_ZONE,
				false, contig, contig);
	if (IS_ERR(rl)) {
		o->err = (int)PTR_ERR(rl);
	} else {
		o->err = 0;
		for (struct runlist_element *r = rl; r->length; r++) {
			if (o->nruns < MAX_RUNS) {
				o->run[o->nruns].vcn = r->vcn;
				o->run[o->nruns].lcn = r->lcn;
				o->run[o->nruns].len = r->length;
			}
			o->nruns++;
		}
		CHECK(ntfs_cluster_free_from_rl(h->vol, rl) == 0);
		kvfree(rl);
	}
	if (pr) {
		CHECK(ntfs_cluster_free_from_rl(h->vol, pr) == 0);
		kvfree(pr);
	}
	umount_vol(dev, h);
	o->bitmap_back = bitmap_io(after, false) == 0 && !memcmp(after, crafted, bmp.len);
	free(after);
}

/*
 * What every successful request must satisfy, whatever the case: runs that
 * follow each other from VCN 0, inside the volume, on clusters that were free
 * and not held by a priming allocation; @count clusters exactly, or fewer for
 * a contiguous request, which may come back short and be asked again for the
 * rest.
 */
static void check_runs(const char *tag, const struct outcome *o, const uint8_t *crafted,
		       s64 count, bool contig)
{
	s64 vcn = 0;

	CHECK_MSG(o->nruns <= MAX_RUNS, "%s: %d runs", tag, o->nruns);
	for (int i = 0; i < o->nruns && i < MAX_RUNS; i++) {
		s64 lcn = o->run[i].lcn, len = o->run[i].len;

		printf("    %s: run %d: vcn %lld lcn %lld length %lld\n", tag, i,
		       (long long)o->run[i].vcn, (long long)lcn, (long long)len);
		CHECK_MSG(o->run[i].vcn == vcn, "%s: run %d starts at vcn %lld, not %lld",
			  tag, i, (long long)o->run[i].vcn, (long long)vcn);
		CHECK_MSG(lcn >= 0 && len > 0 && (u64)(lcn + len) <= bmp.nr_clusters,
			  "%s: run %d (lcn %lld, length %lld) is not inside the volume",
			  tag, i, (long long)lcn, (long long)len);
		for (s64 c = lcn; c >= 0 && c < lcn + len && (u64)c < bmp.nr_clusters; c++) {
			if (bit_set(crafted, (u64)c) || (o->prime_held &&
			    c >= o->prime_lcn && c < o->prime_lcn + o->prime_len)) {
				CHECK_MSG(false, "%s: cluster %lld was not free", tag, (long long)c);
				break;
			}
		}
		vcn += len;
	}
	if (contig)
		CHECK_MSG(vcn > 0 && vcn <= count, "%s: %lld of %lld clusters", tag,
			  (long long)vcn, (long long)count);
	else
		CHECK_MSG(vcn == count, "%s: %lld of %lld clusters", tag,
			  (long long)vcn, (long long)count);
	CHECK_MSG(o->bitmap_back, "%s: $Bitmap differs from the crafted one after the "
		  "request was freed and the volume unmounted", tag);
}

/*
 * Four requests, each on a fresh mount of an 8 MB volume (2047 clusters, MFT
 * zone [0, 259), data zone [259, 2047)), each over a $Bitmap that leaves free
 * only what the case needs. On the pinned mkntfs, clusters 259-360 (the root
 * folder's security descriptor, $Bitmap, $Secure, $UpCase) and 1023-1535
 * ($MFTMirr, $LogFile) are in use; 259 is also the zone position a fresh mount
 * starts from. Measured on 0.4.2's allocator, then with 18d4d2a3ce75 alone,
 * then with both:
 *
 *   A  contiguous, 20 clusters, hint 4 clusters before the end of the volume,
 *      zone position on free space: 2 runs / 1 / 1
 *   B  contiguous, 20 clusters, no hint, the data zone full but for its last
 *      4 clusters: 2 runs / 2 runs / 1
 *   C  not contiguous, 8 clusters, hint at the end of the volume, 100 clusters
 *      free in the middle: -ENOSPC / 8 / 8
 *   D  not contiguous, 8 clusters, hint far past the end of the bitmap, 50
 *      clusters free before the zone position: -ENOSPC / 8 / 8
 *
 * A, C and D need 18d4d2a3ce75; B needs b05a67615b98.
 */
static void test_alloc_cases(void)
{
	const u64 NR = 2047, ZONE = 259;
	uint8_t *orig = NULL, *a = NULL;
	struct outcome o;

	if (make_image("cases", 8) != 0)
		return;
	if (locate_bitmap() != 0) {
		CHECK_MSG(false, "could not find $Bitmap on the image");
		goto out;
	}
	orig = malloc(bmp.len);
	a = malloc(bmp.len);
	if (!orig || !a || bitmap_io(orig, false) != 0) {
		CHECK_MSG(false, "could not read $Bitmap");
		goto out;
	}
	/* The MFT zone ends 1/8 of the volume past $MFT (ntfs_setup_allocators()). */
	CHECK_MSG(bmp.nr_clusters == NR && bmp.mft_lcn + (NR >> 3) == ZONE &&
		  bit_set(orig, ZONE),
		  "fixture layout moved: %" PRIu64 " clusters, $MFT at %" PRIu64
		  ", cluster %" PRIu64 " %s", bmp.nr_clusters, bmp.mft_lcn, ZONE,
		  bit_set(orig, ZONE) ? "used" : "free");
	if (bmp.nr_clusters != NR || bmp.mft_lcn + (NR >> 3) != ZONE)
		goto out;

	/*
	 * A. A contiguous request from a hint four clusters before the end of the
	 * volume: the run from the hint stops at the zone's end. 0.4.2 then went
	 * to the zone position, found it free, and started a second run there.
	 * The zone position is moved onto free space first, as any write would
	 * have moved it: a fresh mount's position, 259, is in use, and a cluster
	 * in use ends a contiguous run, so 0.4.2 stopped there correctly.
	 */
	memcpy(a, orig, bmp.len);
	mark_used(a, ZONE, NR);
	if (!mark_free(a, orig, 600, 700) || !mark_free(a, orig, NR - 4, NR))
		goto out;
	request(a, 1, false, 20, NR - 4, true, &o);
	CHECK_MSG(o.prime_lcn == 600, "A: the priming allocation went to %lld, not 600",
		  (long long)o.prime_lcn);
	CHECK_MSG(o.err == 0, "A: %d", o.err);
	if (!o.err) {
		check_runs("A", &o, a, 20, true);
		CHECK_MSG(o.nruns == 1, "A: a contiguous request came back as %d runs "
			  "(18d4d2a3ce75)", o.nruns);
	}

	/*
	 * B. The same request with no hint: its run is the longest free stretch
	 * in the data zone, the last four clusters, and stops at the zone's end.
	 * 0.4.2 then searched the other zones, shrank the MFT zone and took the
	 * first free cluster at its new end, 129, for a second run.
	 */
	memcpy(a, orig, bmp.len);
	mark_used(a, ZONE, NR);
	if (!mark_free(a, orig, NR - 4, NR))
		goto out;
	request(a, 0, false, 20, -1, true, &o);
	CHECK_MSG(o.err == 0, "B: %d", o.err);
	if (!o.err) {
		check_runs("B", &o, a, 20, true);
		CHECK_MSG(o.nruns == 1, "B: a contiguous request came back as %d runs "
			  "(b05a67615b98)", o.nruns);
	}

	/*
	 * C. The false -ENOSPC. The hint is the end of the volume, as for an
	 * attribute whose last run ends on the last cluster; 100 clusters are free
	 * in the middle and nothing else is. 0.4.2 tried the one cluster at the
	 * zone position, skipped the rest of the page because it was in use, and
	 * shrank the MFT zone to nothing looking elsewhere.
	 */
	memcpy(a, orig, bmp.len);
	mark_used(a, 0, NR);
	if (!mark_free(a, orig, 700, 800))
		goto out;
	request(a, 0, false, 8, NR, false, &o);
	CHECK_MSG(o.err == 0, "C: failed with %d while %lld clusters were free "
		  "(18d4d2a3ce75)", o.err, (long long)o.free_seen);
	if (!o.err)
		check_runs("C", &o, a, 8, false);

	/*
	 * D. A hint past the end of the bitmap, as ntfs_attr_map_cluster()
	 * extrapolates for a write far into a sparse file. The search started in
	 * pass 2, whose range ends at the hint; 0.4.2 kept that pass when it moved
	 * to the zone position and so never scanned what lies before it. The
	 * priming allocation takes [900, 1001) and moves the zone position to
	 * 1001; the 50 free clusters are at [400, 450).
	 */
	memcpy(a, orig, bmp.len);
	mark_used(a, 0, NR);
	if (!mark_free(a, orig, 400, 450) || !mark_free(a, orig, 900, 1001))
		goto out;
	request(a, 101, true, 8, 100000, false, &o);
	CHECK_MSG(o.prime_lcn == 900 && o.prime_len == 101,
		  "D: the priming allocation went to %lld+%lld, not 900+101",
		  (long long)o.prime_lcn, (long long)o.prime_len);
	CHECK_MSG(o.err == 0, "D: failed with %d while %lld clusters were free "
		  "(18d4d2a3ce75)", o.err, (long long)o.free_seen);
	if (!o.err)
		check_runs("D", &o, a, 8, false);

out:
	/* Put mkntfs's bitmap back: nothing else on the volume was touched. */
	if (orig && bmp.len && bitmap_io(orig, true) == 0)
		check_image("cases");
	else
		CHECK_MSG(false, "could not restore $Bitmap");
	free(orig);
	free(a);
	drop_image();
	printf("test_alloc_cases\n");
}

/* ====================================================================== */
/* 3. upstream's symptom, kept as a guard                                 */
/* ====================================================================== */

/* The LCN of the first cluster that starts with @tag, or UINT64_MAX. */
static uint64_t find_tag(uint32_t tag)
{
	uint8_t b[4];
	int fd = open(img, O_RDONLY);

	if (fd < 0)
		return UINT64_MAX;
	for (uint64_t lcn = 0; lcn < bmp.nr_clusters; lcn++) {
		if (pread(fd, b, 4, (off_t)(lcn * bmp.cluster_size)) != 4)
			break;
		if (le_at(b, 4) == tag) {
			close(fd);
			return lcn;
		}
	}
	close(fd);
	return UINT64_MAX;
}

/* Write @fill over every free cluster: what a deleted file leaves behind. */
static int stain_free_clusters(uint8_t fill)
{
	uint8_t *b = malloc(bmp.len), *c = malloc(bmp.cluster_size);
	int fd = open(img, O_RDWR), err = 0;

	if (!b || !c || fd < 0 || bitmap_io(b, false)) {
		err = -1;
		goto out;
	}
	memset(c, fill, bmp.cluster_size);
	for (uint64_t lcn = 0; lcn < bmp.nr_clusters && !err; lcn++)
		if (!bit_set(b, lcn) &&
		    pwrite(fd, c, bmp.cluster_size, (off_t)(lcn * bmp.cluster_size)) !=
		    (ssize_t)bmp.cluster_size)
			err = -1;
out:
	if (fd >= 0)
		close(fd);
	free(b);
	free(c);
	return err;
}

/*
 * The data exposure upstream reported for b05a67615b98's defect: fallocate()
 * of 20 clusters over a hole, with the hint 4 clusters before the end of the
 * volume, after which 16 of them read back a deleted file's data.
 *
 * It does not happen in this port, before or after the fixes, and this test
 * passes on 0.4.2. The second run is there: once a write has moved the zone
 * position onto free space, as nudge.bin's does below, 0.4.2's allocator
 * answers this request with 4 clusters at the end of the volume and 16 at the
 * zone position, in one call (case A above). But 7.1's ntfs_attr_fallocate()
 * zeroes every run ntfs_attr_map_cluster() maps, including the second one,
 * which it is then told was already allocated. Mainline zeroes only the runs
 * reported as new, and that is where upstream read the old data. The test is
 * here so that taking mainline's fallocate() without the allocator fixes
 * fails. Measured with mainline's form applied to 0.4.2: 16 of the 20
 * clusters read back 'D'; with 18d4d2a3ce75 as well, none.
 *
 * Every free cluster holds 'D' before the call, written to the image while it
 * is unmounted. The read-back is after a remount; a cached read proves nothing.
 */
static void test_fallocate_hole_zeroes(void)
{
	const int NCLU = 20;
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	uint8_t *buf = NULL;
	uint64_t p, hole, cs;
	int err;

	if (make_image("falloc", 8) != 0)
		return;
	if (locate_bitmap() != 0) {
		CHECK_MSG(false, "could not find $Bitmap on the image");
		goto out;
	}
	cs = bmp.cluster_size;
	buf = calloc(NCLU, cs);
	if (!buf)
		goto out;

	/* One tagged cluster at VCN 0, to find where it went. */
	if ((err = mount_rw(&dev, &vol)) != 0) {
		CHECK_MSG(false, "mount: %d", err);
		goto out;
	}
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_create(root, "sparse.bin", 0100644, &f) == 0);
	memset(buf, 'F', cs);
	memcpy(buf, "\x7a\x11\x0c\xf1", 4);
	CHECK(ntfs_write(f, buf, cs, 0) == (ssize_t)cs);
	ntfs_inode_put(f);
	ntfs_inode_put(root);
	umount_vol(dev, vol);
	p = find_tag(0xf10c117a);
	CHECK_MSG(p != UINT64_MAX && p + 4 < bmp.nr_clusters, "tagged cluster not found");
	if (p == UINT64_MAX || p + 4 >= bmp.nr_clusters)
		goto out;

	/*
	 * ntfs_attr_map_cluster() extrapolates its hint from the run before the
	 * hole: LCN p for VCN 0 gives p + vcn. Put the hole where that is four
	 * clusters before the end of the volume, and end the file with a cluster
	 * after it so that the hole lies inside initialized_size.
	 */
	hole = bmp.nr_clusters - 4 - p;
	if ((err = mount_rw(&dev, &vol)) != 0) {
		CHECK_MSG(false, "mount: %d", err);
		goto out;
	}
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_lookup(root, "sparse.bin", &f) == 0);
	memset(buf, 'T', cs);
	CHECK(ntfs_write(f, buf, cs, (hole + NCLU + 8) * cs) == (ssize_t)cs);
	ntfs_inode_put(f);
	ntfs_inode_put(root);
	umount_vol(dev, vol);

	CHECK_MSG(stain_free_clusters('D') == 0, "could not write the free clusters");

	if ((err = mount_rw(&dev, &vol)) != 0) {
		CHECK_MSG(false, "mount: %d", err);
		goto out;
	}
	CHECK(ntfs_volume_root(vol, &root) == 0);
	/* Any write moves the zone position past what it took, onto free space. */
	CHECK(ntfs_create(root, "nudge.bin", 0100644, &f) == 0);
	memset(buf, 'N', cs);
	CHECK(ntfs_write(f, buf, cs, 0) == (ssize_t)cs);
	ntfs_inode_put(f);
	CHECK(ntfs_lookup(root, "sparse.bin", &f) == 0);
	err = ntfs_fallocate(f, hole * cs, (uint64_t)NCLU * cs, true);
	CHECK_MSG(err == 0, "fallocate of %d clusters over the hole: %d", NCLU, err);
	ntfs_inode_put(f);
	ntfs_inode_put(root);
	umount_vol(dev, vol);

	if ((err = mount_rw(&dev, &vol)) != 0) {
		CHECK_MSG(false, "mount: %d", err);
		goto out;
	}
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_lookup(root, "sparse.bin", &f) == 0);
	memset(buf, 0xee, (size_t)NCLU * cs);
	CHECK(ntfs_read(f, buf, (size_t)NCLU * cs, hole * cs) == (ssize_t)(NCLU * cs));
	{
		int stale = 0, dirty = 0;

		for (int i = 0; i < NCLU; i++) {
			const uint8_t *c = buf + (size_t)i * cs;
			bool nz = false;

			for (uint64_t k = 0; k < cs && !nz; k++)
				nz = c[k] != 0;
			dirty += nz;
			stale += c[cs - 1] == 'D';
		}
		CHECK_MSG(dirty == 0, "%d of %d fallocated clusters read back non-zero after "
			  "a remount, %d of them a deleted file's bytes", dirty, NCLU, stale);
	}
	ntfs_inode_put(f);
	ntfs_inode_put(root);
	umount_vol(dev, vol);
	check_image("falloc");
out:
	free(buf);
	drop_image();
	printf("test_fallocate_hole_zeroes\n");
}

/* ====================================================================== */
/* 4. freeing clusters when $Bitmap cannot be read                        */
/* ====================================================================== */

/* A device whose reads of one byte range fail while armed. */
static const struct ntfs_bdev_ops *real_ops;
static struct ntfs_bdev_ops faulty_ops;
static struct {
	bool armed;
	uint64_t lo, hi;
	int fired;
} rfault;

static ssize_t faulty_pread(struct ntfs_bdev *d, void *b, size_t c, u64 o)
{
	if (rfault.armed && o < rfault.hi && o + c > rfault.lo) {
		rfault.fired++;
		return -EIO;
	}
	return real_ops->pread(d, b, c, o);
}

static int mount_faulty(struct ntfs_bdev **dev, ntfs_volume_t **vol)
{
	int err;

	memset(&rfault, 0, sizeof(rfault));
	*dev = ntfs_bdev_open_path(img, false);
	if (!*dev)
		return -ENOENT;
	real_ops = (*dev)->ops;
	faulty_ops = *real_ops;
	faulty_ops.pread = faulty_pread;
	(*dev)->ops = &faulty_ops;
	err = ntfs_mount(*dev, &RW_OPTS, vol);
	if (err)
		ntfs_bdev_close(*dev);
	return err;
}

/*
 * be9e89ccb8e5, "ntfs: only count successfully cleared runs when freeing
 * clusters" (U16). ntfs_cluster_free_from_rl_nolock() added a run's length to
 * the free count whenever its error bookkeeping did not record the failure,
 * and a second failure with the same errno is not recorded. Two runs of four
 * clusters are allocated over a crafted $Bitmap. The volume is synced so the
 * bitmap page is clean, and the page is dropped from the cache. With reads of
 * $Bitmap failing, freeing both runs fails twice with -EIO, and the free count
 * must not move, because no bit was cleared. 0.4.2 counted the second run.
 * Once the device reads again, the same free succeeds and the count follows
 * the bitmap.
 */
static void test_free_with_unreadable_bitmap(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *h;
	struct runlist_element *rl = NULL;
	struct address_space *bmap;
	uint8_t *orig = NULL, *a = NULL;
	s64 before, after, done;
	int err, nruns = 0;

	if (make_image("freefail", 8) != 0)
		return;
	if (locate_bitmap() != 0) {
		CHECK_MSG(false, "could not find $Bitmap on the image");
		goto out;
	}
	orig = malloc(bmp.len);
	a = malloc(bmp.len);
	if (!orig || !a || bitmap_io(orig, false) != 0) {
		CHECK_MSG(false, "could not read $Bitmap");
		goto out;
	}
	memcpy(a, orig, bmp.len);
	mark_used(a, 0, bmp.nr_clusters);
	if (!mark_free(a, orig, 600, 604) || !mark_free(a, orig, 700, 704))
		goto restore;
	CHECK(bitmap_io(a, true) == 0);

	if ((err = mount_faulty(&dev, &h)) != 0) {
		CHECK_MSG(false, "mount: %d", err);
		goto restore;
	}
	/* From the hint at 600, four clusters; then the zone search finds 700. */
	rl = ntfs_cluster_alloc(h->vol, 0, 8, 600, DATA_ZONE, false, false, false);
	CHECK_MSG(!IS_ERR(rl), "allocating two runs: %ld", IS_ERR(rl) ? PTR_ERR(rl) : 0L);
	if (IS_ERR(rl)) {
		rl = NULL;
		goto unmount;
	}
	for (struct runlist_element *r = rl; r->length; r++)
		nruns++;
	CHECK_MSG(nruns == 2, "the allocation came back as %d runs, not 2", nruns);

	/* Write the bitmap page out, then drop it, so the free has to read it. */
	CHECK(ntfs_volume_sync(h) == 0);
	bmap = h->vol->lcnbmp_ino->i_mapping;
	invalidate_mapping_pages(bmap, 0, ~(pgoff_t)0);
	CHECK_MSG(bmap->nrpages == 0, "$Bitmap still has %lu pages cached",
		  (unsigned long)bmap->nrpages);

	rfault.lo = bmp.off;
	rfault.hi = bmp.off + bmp.cluster_size;
	rfault.armed = true;
	before = atomic64_read(&h->vol->free_clusters);
	err = ntfs_cluster_free_from_rl(h->vol, rl);
	after = atomic64_read(&h->vol->free_clusters);
	rfault.armed = false;
	CHECK_MSG(err < 0 && rfault.fired >= 2, "freeing two runs with $Bitmap unreadable "
		  "returned %d after %d failed reads", err, rfault.fired);
	CHECK_MSG(after == before, "the free count went from %lld to %lld, though "
		  "neither run could be freed (be9e89ccb8e5)", (long long)before,
		  (long long)after);

	/* The device reads again: the same free works and the count is right. */
	CHECK(ntfs_cluster_free_from_rl(h->vol, rl) == 0);
	done = atomic64_read(&h->vol->free_clusters);
	CHECK_MSG(done == before + 8, "after the retry the free count is %lld, not %lld",
		  (long long)done, (long long)(before + 8));
unmount:
	kvfree(rl);
	umount_vol(dev, h);
restore:
	/* Put mkntfs's bitmap back: nothing else on the volume was touched. */
	if (bitmap_io(orig, true) == 0)
		check_image("freefail");
	else
		CHECK_MSG(false, "could not restore $Bitmap");
out:
	free(orig);
	free(a);
	drop_image();
	printf("test_free_with_unreadable_bitmap\n");
}

/* ====================================================================== */
/* 5. crafted images: upstream 19cac7902a8a                               */
/* ====================================================================== */

/*
 * 19cac7902a8a, "ntfs: bound the free-cluster bitmap scan to the volume"
 * (U15), its runlist.c half: ntfs_mapping_pairs_decompress() accepted an LCN
 * at or past the end of the volume. Here a file's one run is pointed at LCN
 * nr_clusters. On these images that cluster is still on the device: it is the
 * slack after the last cluster, and it holds the backup boot sector. A write
 * to the file must be refused, and those bytes must stay as they were.
 * Measured without the fix: the write returned 4096 and overwrote them.
 */
static void test_run_past_volume_end(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	uint8_t rec[4096], *buf = NULL, *before = NULL, *after = NULL;
	uint64_t ino = 0, cs, end_lcn;
	const uint8_t *mp_end;
	uint8_t *mp;
	ssize_t wn = 0;
	int64_t at;
	uint32_t a;
	int err, nl, no;

	if (make_image("pastend", 8) != 0)
		return;
	if (read_boot() != 0) {
		CHECK_MSG(false, "could not read the boot sector");
		goto out;
	}
	cs = bmp.cluster_size;
	end_lcn = bmp.nr_clusters;
	buf = malloc(cs);
	before = malloc(cs);
	after = malloc(cs);
	if (!buf || !before || !after)
		goto out;

	if ((err = mount_rw(&dev, &vol)) != 0) {
		CHECK_MSG(false, "mount: %d", err);
		goto out;
	}
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_create(root, "outside.bin", 0100644, &f) == 0);
	memset(buf, 'O', cs);
	CHECK(ntfs_write(f, buf, cs, 0) == (ssize_t)cs);
	ino = ntfs_inode_number(f);
	ntfs_inode_put(f);
	ntfs_inode_put(root);
	umount_vol(dev, vol);

	/* One run of one cluster, its LCN in two bytes: rewrite the LCN in place. */
	at = read_record(ino, rec);
	a = at < 0 ? 0 : find_attr(rec, 0x80);
	CHECK_MSG(a && rec[a + 8], "outside.bin: no non-resident $DATA in record %" PRIu64, ino);
	if (!a || !rec[a + 8])
		goto out;
	mp = rec + a + le_at(rec + a + 0x20, 2);
	mp_end = rec + a + le_at(rec + a + 4, 4);
	nl = mp[0] & 15;
	no = mp[0] >> 4;
	CHECK_MSG(nl && no == 2 && mp + 2 + nl + no <= mp_end && le_at(mp + 1, nl) == 1 &&
		  !mp[1 + nl + no] && end_lcn < 0x8000,
		  "outside.bin: its run list is not the one-run shape this case rewrites");
	if (!(nl && no == 2 && mp + 2 + nl + no <= mp_end && le_at(mp + 1, nl) == 1 &&
	      !mp[1 + nl + no] && end_lcn < 0x8000))
		goto out;
	put_le(mp + 1 + nl, end_lcn, 2);
	CHECK(write_record(at, rec) == 0);

	CHECK(image_io(before, cs, end_lcn * cs, false) == 0);
	CHECK_MSG(!memcmp(before + cs - 512 + 3, "NTFS    ", 8),
		  "no backup boot sector at the end of LCN %" PRIu64, end_lcn);

	if ((err = mount_rw(&dev, &vol)) != 0) {
		CHECK_MSG(false, "mount: %d", err);
		goto out;
	}
	CHECK(ntfs_volume_root(vol, &root) == 0);
	memset(buf, 'X', cs);
	err = ntfs_lookup(root, "outside.bin", &f);
	if (!err) {
		wn = ntfs_write(f, buf, cs, 0);
		ntfs_inode_put(f);
	} else {
		wn = err;
	}
	ntfs_inode_put(root);
	umount_vol(dev, vol);
	CHECK_MSG(wn < 0, "a write through a run at LCN %" PRIu64 ", past the end of the "
		  "volume, returned %zd", end_lcn, wn);
	CHECK(image_io(after, cs, end_lcn * cs, false) == 0);
	CHECK_MSG(!memcmp(before, after, cs), "a write through a run at LCN %" PRIu64
		  ", past the end of the volume, changed the device there: the backup "
		  "boot sector", end_lcn);
	/* No ntfsck: the run list is corrupt by construction. */
out:
	free(buf);
	free(before);
	free(after);
	drop_image();
	printf("test_run_past_volume_end\n");
}

static void bigbitmap_fault(int sig)
{
	static const char msg[] =
		"FAIL test_alloc.c: test_bitmap_longer_than_volume: ntfs_cluster_alloc() "
		"read vol->lcn_empty_bits_per_page past its end, into the guard page "
		"(19cac7902a8a, U15). Ending the run here.\n";

	(void)sig;
	(void)!write(2, msg, sizeof(msg) - 1);
	_exit(1);
}

/*
 * 19cac7902a8a's lcnalloc.c half. vol->lcn_empty_bits_per_page is sized from
 * nr_clusters at mount, one entry per page of bitmap. ntfs_cluster_alloc()
 * bounded its scan by $Bitmap's size instead, and the mount refuses only a
 * $Bitmap that is too short. Here $Bitmap is moved to two clusters, which
 * cover 65536 clusters on a 2047-cluster volume, so the array has one entry
 * and the bitmap two pages. An fallocate far into a sparse file makes
 * ntfs_attr_map_cluster() extrapolate a hint in the second page, and the scan
 * reads entry 1. Upstream saw it with KASAN; ASan does not run on this
 * platform (docs/TESTING.md), so the array is copied to the end of a page with
 * an inaccessible page after it, and the read faults. The fault handler
 * reports it and ends the run, which is why this case comes last.
 */
static void test_bitmap_longer_than_volume(void)
{
	const uint64_t HINT = 32768 + 1000;	/* in the second page of the bitmap */
	struct ntfs_bdev *dev;
	ntfs_volume_t *h;
	ntfs_inode_t *root = NULL, *f = NULL;
	struct ntfs_volume_info info;
	struct sigaction sa, old_segv, old_bus;
	uint8_t rec[4096], *page1 = NULL, *page2 = NULL, *buf = NULL, *mp;
	unsigned int *orig, *arr;
	uint64_t cs, nr, old_lcn, new_lcn = 0, p, n;
	size_t ps;
	void *base;
	int64_t at;
	uint32_t a;
	int err, nl, no;

	if (make_image("bigbitmap", 8) != 0)
		return;
	if (locate_bitmap() != 0) {
		CHECK_MSG(false, "could not find $Bitmap on the image");
		goto out;
	}
	cs = bmp.cluster_size;
	nr = bmp.nr_clusters;
	old_lcn = bmp.off / cs;
	page1 = malloc(cs);
	page2 = malloc(cs);
	buf = malloc(cs);
	if (!page1 || !page2 || !buf || image_io(page1, cs, bmp.off, false))
		goto out;

	/* Two free clusters in the data zone to move $Bitmap to. */
	for (uint64_t c = 400; c + 1 < nr && !new_lcn; c++)
		if (!bit_set(page1, c) && !bit_set(page1, c + 1))
			new_lcn = c;
	CHECK_MSG(new_lcn && new_lcn < 0x8000 && 2 * cs * 8 > HINT,
		  "no room to move $Bitmap to two clusters");
	if (!new_lcn || new_lcn >= 0x8000 || 2 * cs * 8 <= HINT)
		goto out;
	page1[old_lcn >> 3] &= (uint8_t)~(1u << (old_lcn & 7));
	mark_used(page1, new_lcn, new_lcn + 2);
	memset(page2, 0xff, cs);	/* clusters past the end: in use */
	CHECK(image_io(page1, cs, new_lcn * cs, true) == 0);
	CHECK(image_io(page2, cs, (new_lcn + 1) * cs, true) == 0);

	at = read_record(6, rec);
	a = at < 0 ? 0 : find_attr(rec, 0x80);
	if (!a || !rec[a + 8]) {
		CHECK_MSG(false, "$Bitmap has no non-resident $DATA");
		goto out;
	}
	mp = rec + a + le_at(rec + a + 0x20, 2);
	nl = mp[0] & 15;
	no = mp[0] >> 4;
	CHECK_MSG(nl && no == 2 && !mp[1 + nl + no], "$Bitmap's run list has moved shape");
	if (!(nl && no == 2 && !mp[1 + nl + no]))
		goto out;
	put_le(mp + 1, 2, nl);			/* two clusters */
	put_le(mp + 1 + nl, new_lcn, 2);	/* at new_lcn */
	put_le(rec + a + 0x18, 1, 8);		/* highest VCN */
	put_le(rec + a + 0x28, 2 * cs, 8);	/* allocated size */
	put_le(rec + a + 0x30, 2 * cs, 8);	/* data size */
	put_le(rec + a + 0x38, 2 * cs, 8);	/* initialized size */
	CHECK(write_record(at, rec) == 0);

	/* A file with its first cluster at LCN p, to extrapolate the hint from. */
	if ((err = mount_rw(&dev, &h)) != 0) {
		CHECK_MSG(false, "mount with a $Bitmap longer than the volume: %d", err);
		goto out;
	}
	CHECK(ntfs_volume_root(h, &root) == 0);
	CHECK(ntfs_create(root, "sparse.bin", 0100644, &f) == 0);
	memset(buf, 'F', cs);
	memcpy(buf, "\x7a\x11\x0c\xf1", 4);
	CHECK(ntfs_write(f, buf, cs, 0) == (ssize_t)cs);
	ntfs_inode_put(f);
	ntfs_inode_put(root);
	umount_vol(dev, h);
	p = find_tag(0xf10c117a);
	CHECK_MSG(p < nr, "tagged cluster not found");
	if (p >= nr)
		goto out;

	if ((err = mount_rw(&dev, &h)) != 0) {
		CHECK_MSG(false, "mount: %d", err);
		goto out;
	}
	CHECK(ntfs_volume_root(h, &root) == 0);
	err = ntfs_lookup(root, "sparse.bin", &f);
	CHECK_MSG(err == 0, "sparse.bin: %d", err);
	if (err) {
		f = NULL;
		goto unmount;
	}
	CHECK(ntfs_volume_get_info(h, &info) == 0);
	CHECK(NVolFreeClusterKnown(h->vol));

	/* The array, moved against a guard page. */
	n = (((nr + 7) >> 3) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	ps = (size_t)sysconf(_SC_PAGESIZE);
	base = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	CHECK(base != MAP_FAILED);
	if (base == MAP_FAILED)
		goto unmount;
	CHECK(mprotect((uint8_t *)base + ps, ps, PROT_NONE) == 0);
	arr = (unsigned int *)((uint8_t *)base + ps) - n;
	orig = h->vol->lcn_empty_bits_per_page;
	memcpy(arr, orig, n * sizeof(*arr));
	h->vol->lcn_empty_bits_per_page = arr;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = bigbitmap_fault;
	sigaction(SIGSEGV, &sa, &old_segv);
	sigaction(SIGBUS, &sa, &old_bus);
	fflush(stdout);		/* the handler leaves through _exit() */
	fflush(stderr);

	err = ntfs_fallocate(f, (HINT - p) * cs, cs, true);

	sigaction(SIGSEGV, &old_segv, NULL);
	sigaction(SIGBUS, &old_bus, NULL);
	memcpy(orig, arr, n * sizeof(*arr));
	h->vol->lcn_empty_bits_per_page = orig;
	munmap(base, 2 * ps);
	CHECK_MSG(err == 0, "fallocate far into a sparse file, on a volume whose $Bitmap "
		  "is longer than the volume: %d", err);
unmount:
	if (f)
		ntfs_inode_put(f);
	ntfs_inode_put(root);
	umount_vol(dev, h);
	/* No ntfsck: $Bitmap is longer than the volume by construction. */
out:
	free(page1);
	free(page2);
	free(buf);
	drop_image();
	printf("test_bitmap_longer_than_volume\n");
}

/* A case that bails out early still owns an image; do not litter CI's /tmp. */
static void cleanup(void) { drop_image(); }

int main(void)
{
	atexit(cleanup);
	test_names_wall();
	test_alloc_cases();
	test_fallocate_hole_zeroes();
	test_free_with_unreadable_bitmap();
	test_run_past_volume_end();
	test_bitmap_longer_than_volume();	/* last: a fault ends the run */
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
