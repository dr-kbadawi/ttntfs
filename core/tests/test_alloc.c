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
 * Every case ends with ntfsck -n on the image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>

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

/* ---- the image's $Bitmap, read and written raw while unmounted -------- */

static struct {
	uint32_t cluster_size;
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

/*
 * Find $Bitmap's data: boot sector, then MFT record 6, then its unnamed $DATA.
 * The volumes here are small enough that it is one run; anything else is
 * reported rather than guessed at.
 */
static int locate_bitmap(void)
{
	uint8_t bs[512], rec[4096];
	uint32_t rec_size, off;
	int8_t cpr;
	int fd = open(img, O_RDONLY);

	if (fd < 0)
		return -1;
	if (pread(fd, bs, sizeof(bs), 0) != (ssize_t)sizeof(bs))
		goto bad;
	bmp.cluster_size = (uint32_t)le_at(bs + 0x0b, 2) * bs[0x0d];
	bmp.nr_clusters = le_at(bs + 0x28, 8) * le_at(bs + 0x0b, 2) / bmp.cluster_size;
	bmp.mft_lcn = le_at(bs + 0x30, 8);
	cpr = (int8_t)bs[0x40];
	rec_size = cpr > 0 ? (uint32_t)cpr * bmp.cluster_size : 1u << -cpr;
	if (rec_size > sizeof(rec) ||
	    pread(fd, rec, rec_size, (off_t)(bmp.mft_lcn * bmp.cluster_size + 6 * rec_size)) !=
	    (ssize_t)rec_size)
		goto bad;
	close(fd);
	{	/* undo the update sequence fixups */
		uint32_t usa = (uint32_t)le_at(rec + 4, 2), n = (uint32_t)le_at(rec + 6, 2);

		for (uint32_t i = 1; i < n && i * 512 <= rec_size; i++)
			memcpy(rec + i * 512 - 2, rec + usa + 2 * i, 2);
	}
	for (off = (uint32_t)le_at(rec + 0x14, 2); off + 0x40 <= rec_size;
	     off += (uint32_t)le_at(rec + off + 4, 4)) {
		uint32_t type = (uint32_t)le_at(rec + off, 4);
		const uint8_t *mp;
		uint64_t len;
		int nl, no;

		if (type == 0xffffffff || !le_at(rec + off + 4, 4))
			break;
		if (type != 0x80 || !rec[off + 8] || rec[off + 9])
			continue;
		mp = rec + off + le_at(rec + off + 0x20, 2);
		nl = mp[0] & 15;
		no = mp[0] >> 4;
		len = le_at(mp + 1, nl);
		bmp.off = le_at(mp + 1 + nl, no) * bmp.cluster_size;
		bmp.len = (size_t)le_at(rec + off + 0x30, 8);
		if (mp[1 + nl + no] || len * bmp.cluster_size < bmp.len)
			return -1;
		return 0;
	}
	return -1;
bad:
	close(fd);
	return -1;
}

static int bitmap_io(uint8_t *buf, bool write)
{
	int fd = open(img, O_RDWR);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write ? pwrite(fd, buf, bmp.len, (off_t)bmp.off)
		  : pread(fd, buf, bmp.len, (off_t)bmp.off);
	close(fd);
	return n == (ssize_t)bmp.len ? 0 : -1;
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

/* A case that bails out early still owns an image; do not litter CI's /tmp. */
static void cleanup(void) { drop_image(); }

int main(void)
{
	atexit(cleanup);
	test_names_wall();
	test_alloc_cases();
	test_fallocate_hole_zeroes();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
