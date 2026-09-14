// SPDX-License-Identifier: GPL-2.0
/*
 * test_crash.c - crash consistency: what an interrupted volume looks like.
 *
 * The port drives no $LogFile, so nothing rolls a half-finished operation
 * back. All that stands between an interrupted write and an unrecoverable
 * volume is the order the writes reach the device in, decided in
 * platform/src/inode.c (sync_rank/cmp_sync_order) and written down in
 * docs/progress/platform-review.md:
 *
 *   bitmap bit set, MFT record not written -> leaked space, chkdsk reclaims;
 *   MFT record written, bitmap bit clear   -> the next allocation hands those
 *                                             clusters to another file and
 *                                             overwrites live data.
 *
 * One is a lost afternoon of disk space, the other is lost data, and the
 * ordering is the only thing keeping the port on the first side. Until this
 * file, nothing tested that it achieved anything.
 *
 * Three kinds of check here:
 *
 *   ordering_invariant   record every device write during a sync, map the
 *                        offsets back to $Bitmap / $MFT through the boot
 *                        sector geometry, and assert the bitmap update for a
 *                        new file's clusters reaches the device before the
 *                        MFT record that points at them. This is the check
 *                        that actually pins the design: reverse the sort in
 *                        snapshot_inodes() and it fails.
 *   cut_points           run create / write / rename / delete against a block
 *                        device that stops accepting writes after N of them,
 *                        then ask ntfsck what is left. Every cut point is
 *                        recorded with its verdict rather than merely passed
 *                        or failed, because "some cut points are not clean"
 *                        may be the truth and is worth knowing exactly.
 *   allocation_covered   the specific unrecoverable shape: a file whose $DATA
 *                        runs point at clusters whose $Bitmap bits are clear.
 *                        Checked directly against the on-disk bitmap, not via
 *                        ntfsck, so the failure mode is named rather than
 *                        inferred.
 *
 * Every crashed image is also re-mounted, because an image ntfsck likes but
 * the driver cannot open is no better than a corrupt one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ntfscore.h"
#include "ntfsport/bdev.h"
#include "ntfs_image.h"

static int failures, checks;
#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

#define FAILF(...) do {							\
	checks++; failures++;						\
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);		\
	fprintf(stderr, __VA_ARGS__);					\
} while (0)

/* ---- external tools -------------------------------------------------- */

static char mkntfs_path[1024], ntfsck_path[1024];
static char tmpdir[1024];

static void tool_paths(void)
{
	const char *m = getenv("NTFS_MKNTFS"), *c = getenv("NTFS_NTFSCK");
	const char *t = getenv("TMPDIR");

	snprintf(mkntfs_path, sizeof(mkntfs_path), "%s",
		 m && *m ? m : "tools/.local/sbin/mkntfs");
	snprintf(ntfsck_path, sizeof(ntfsck_path), "%s",
		 c && *c ? c : "tools/.local-plus/sbin/ntfsck");
	snprintf(tmpdir, sizeof(tmpdir), "%s", t && *t ? t : "/tmp");
	if (tmpdir[0] && tmpdir[strlen(tmpdir) - 1] == '/')
		tmpdir[strlen(tmpdir) - 1] = '\0';
}

static void scratch_path(char *out, size_t n, const char *tag)
{
	snprintf(out, n, "%s/ttntfs-crash-%d-%s.img", tmpdir, (int)getpid(), tag);
}

/* Build a fresh 16 MiB NTFS volume at @path. */
static int make_image(const char *path)
{
	char cmd[2600];
	int rc;

	snprintf(cmd, sizeof(cmd),
		 "dd if=/dev/zero of='%s' bs=1048576 count=16 >/dev/null 2>&1 && "
		 "'%s' -Q -F -L CRASH '%s' >/dev/null 2>&1",
		 path, mkntfs_path, path);
	rc = system(cmd);
	return (rc == 0) ? 0 : -EIO;
}

static int copy_file(const char *src, const char *dst)
{
	char buf[1 << 16];
	ssize_t n;
	int in, out;

	in = open(src, O_RDONLY);
	if (in < 0)
		return -errno;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) { close(in); return -errno; }
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n) { close(in); close(out); return -EIO; }
	close(in);
	close(out);
	return 0;
}

/*
 * ntfsck's verdict, reduced to something a table can hold. The distinction
 * that matters is clean (nothing to do) versus repairable (chkdsk/ntfsck can
 * put it back) versus refused (it could not even read the volume).
 */
enum verdict { V_CLEAN = 0, V_ERRORS, V_REFUSED, V_SKIPPED };
static const char *verdict_name[] = { "clean", "errors", "refused", "skipped" };

static enum verdict run_ntfsck(const char *path, char *msg, size_t msglen)
{
	char cmd[2600], line[512];
	FILE *p;
	int rc;

	if (msg && msglen)
		msg[0] = '\0';
	/* -n: report only, never write. Anything it would fix is left for us
	 * to look at. */
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' 2>&1", ntfsck_path, path);
	p = popen(cmd, "r");
	if (!p)
		return V_SKIPPED;
	while (fgets(line, sizeof(line), p)) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		/* Keep the first line that names something. The progress
		 * chatter, the per-parse counters and the final "Clean," line
		 * say nothing about which structure is broken. */
		if (!msg || !msglen || msg[0])
			continue;
		if (strncmp(line, "Parse #", 7) == 0 || strstr(line, "percent completed") ||
		    strstr(line, "errors:0"))
			continue;
		if (strstr(line, "rror") || strstr(line, "orrupt") ||
		    strstr(line, "nvalid") || strstr(line, "ailed") ||
		    strstr(line, "ismatch") || strstr(line, "match") ||
		    strstr(line, "Bad") || strstr(line, "Fix it")) {
			char *q = strstr(line, " Fix it?");	/* the prompt is not the finding */
			if (q)
				*q = '\0';
			snprintf(msg, msglen, "%s", line);
		}
	}
	rc = pclose(p);
	if (rc == -1)
		return V_SKIPPED;
	if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0)
		return V_CLEAN;
	if (WIFEXITED(rc) && WEXITSTATUS(rc) < 8)
		return V_ERRORS;
	return V_REFUSED;
}

/* ---- the crashing block device --------------------------------------- */

/*
 * A power cut, modelled at write granularity: the first @limit pwrites land,
 * everything after is acknowledged and dropped. Acknowledged rather than
 * failed on purpose -- a device that has lost power does not report an error
 * to the driver, it simply stops, and returning -EIO would send the driver
 * down its error path instead of the one a real crash takes.
 */
struct trace_ent {
	u64 off;
	size_t len;
};

#define TRACE_MAX 4096
static struct trace_ent trace[TRACE_MAX];
static size_t trace_n;
static bool tracing;

static long write_count;
static long write_limit = LONG_MAX;
static const struct ntfs_bdev_ops *real_ops;
static struct ntfs_bdev_ops crash_ops;

static ssize_t cr_pread(struct ntfs_bdev *d, void *b, size_t c, u64 o)
{
	return real_ops->pread(d, b, c, o);
}

static ssize_t cr_pwrite(struct ntfs_bdev *d, const void *b, size_t c, u64 o)
{
	if (tracing && trace_n < TRACE_MAX) {
		trace[trace_n].off = o;
		trace[trace_n].len = c;
		trace_n++;
	}
	if (write_count++ >= write_limit)
		return (ssize_t)c;		/* the lights went out */
	return real_ops->pwrite(d, b, c, o);
}

static int cr_flush(struct ntfs_bdev *d)
{
	if (write_count >= write_limit)
		return 0;
	return real_ops->flush(d);
}

static int cr_discard(struct ntfs_bdev *d, u64 o, u64 l)
{
	if (write_count >= write_limit)
		return 0;
	return real_ops->discard ? real_ops->discard(d, o, l) : 0;
}

static void cr_close(struct ntfs_bdev *d) { real_ops->close(d); }

static struct ntfs_bdev *crash_bdev_open(const char *path, long limit)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);

	if (!dev)
		return NULL;
	real_ops = dev->ops;
	crash_ops = *real_ops;
	crash_ops.pread = cr_pread;
	crash_ops.pwrite = cr_pwrite;
	crash_ops.flush = cr_flush;
	crash_ops.discard = real_ops->discard ? cr_discard : NULL;
	crash_ops.close = cr_close;
	dev->ops = &crash_ops;
	write_count = 0;
	write_limit = limit;
	return dev;
}

/* ---- geometry: which file owns a byte offset -------------------------- */

enum region { R_OTHER = 0, R_BOOT, R_MFT, R_MFTMIRR, R_BITMAP, R_LOGFILE, R_ROOTIDX };
static const char *region_name[] = { "other", "boot", "$MFT", "$MFTMirr",
				     "$Bitmap", "$LogFile", "root-index" };

struct extent { u64 start, end; int region; };

struct geometry {
	struct ntfs_image img;		/* holds cluster size, $MFT and $LogFile runs */
	bool have;
	struct extent ext[64];
	size_t n_ext;
	struct ntfs_img_run *bitmap_runs;
	uint32_t n_bitmap_runs;
	u64 bitmap_size;
};

static void add_extent(struct geometry *g, u64 start, u64 len, int region)
{
	if (!len || g->n_ext >= sizeof(g->ext) / sizeof(g->ext[0]))
		return;
	g->ext[g->n_ext].start = start;
	g->ext[g->n_ext].end = start + len;
	g->ext[g->n_ext].region = region;
	g->n_ext++;
}

/* Undo the update sequence array so attribute offsets read true. */
static void apply_fixup(uint8_t *rec, uint32_t size, uint32_t sector)
{
	uint16_t usa_ofs = (uint16_t)(rec[4] | (rec[5] << 8));
	uint16_t usa_cnt = (uint16_t)(rec[6] | (rec[7] << 8));
	uint32_t i;

	if (!usa_cnt || usa_ofs + 2u * usa_cnt > size)
		return;
	for (i = 1; i < usa_cnt; i++) {
		uint32_t at = i * sector - 2;
		if (at + 2 > size)
			break;
		rec[at] = rec[usa_ofs + 2 * i];
		rec[at + 1] = rec[usa_ofs + 2 * i + 1];
	}
}

static int read_record(struct geometry *g, u64 no, uint8_t *rec)
{
	u64 off = ntfs_image_map(g->img.mft_runs, g->img.n_mft_runs,
				 g->img.cluster_size, no * g->img.mft_record_size);

	if (off == UINT64_MAX)
		return -ENOENT;
	if (pread(g->img.fd, rec, g->img.mft_record_size, (off_t)off) !=
	    (ssize_t)g->img.mft_record_size)
		return -EIO;
	if (memcmp(rec, "FILE", 4) != 0)
		return -EINVAL;
	apply_fixup(rec, g->img.mft_record_size, g->img.sector_size);
	return 0;
}

/* Mapping-pairs decoder. ntfs_image.c has one but keeps it static, and this
 * needs runs from records it does not parse ($Bitmap, and any user file). */
static int decode_runs(const uint8_t *mp, uint32_t mp_len, u64 svcn,
		       struct ntfs_img_run **out, uint32_t *n)
{
	u64 vcn = svcn;
	int64_t lcn = 0;
	uint32_t i = 0, cap = 0;

	*out = NULL;
	*n = 0;
	while (i < mp_len && mp[i]) {
		uint8_t h = mp[i++], ls = h & 0xf, os = h >> 4;
		u64 len = 0;
		int64_t d = 0;
		uint32_t k;

		if (!ls || ls > 8 || os > 8 || i + ls + os > mp_len)
			return -EINVAL;
		for (k = 0; k < ls; k++)
			len |= (u64)mp[i + k] << (8 * k);
		i += ls;
		if (os) {
			for (k = 0; k < os; k++)
				d |= (int64_t)mp[i + k] << (8 * k);
			if (mp[i + os - 1] & 0x80)
				d |= -((int64_t)1 << (8 * os));
			i += os;
			lcn += d;
			if (lcn < 0)
				return -EINVAL;
		}
		if (*n == cap) {
			struct ntfs_img_run *nr;
			cap = cap ? cap * 2 : 8;
			nr = realloc(*out, cap * sizeof(*nr));
			if (!nr)
				return -ENOMEM;
			*out = nr;
		}
		(*out)[*n].vcn = vcn;
		(*out)[*n].lcn = os ? (u64)lcn : UINT64_MAX;
		(*out)[*n].len = len;
		(*n)++;
		vcn += len;
	}
	return 0;
}

/* Non-resident runs of attribute @type (unnamed) in @rec, plus its data size. */
static int attr_runs(const uint8_t *rec, uint32_t rec_size, uint32_t type,
		     struct ntfs_img_run **runs, uint32_t *n, u64 *data_size)
{
	uint32_t off = (uint32_t)(rec[0x14] | (rec[0x15] << 8));

	*runs = NULL;
	*n = 0;
	if (data_size)
		*data_size = 0;
	while (off + 8 <= rec_size) {
		uint32_t t = (uint32_t)(rec[off] | (rec[off + 1] << 8) |
					(rec[off + 2] << 16) | (rec[off + 3] << 24));
		uint32_t len = (uint32_t)(rec[off + 4] | (rec[off + 5] << 8) |
					  (rec[off + 6] << 16) | (rec[off + 7] << 24));

		if (t == 0xffffffffu || !len || off + len > rec_size)
			break;
		if (t == type && rec[off + 8]) {		/* non-resident */
			uint32_t mp_ofs = (uint32_t)(rec[off + 0x20] | (rec[off + 0x21] << 8));
			u64 lo = 0;
			int i;

			for (i = 0; i < 8; i++)
				lo |= (u64)rec[off + 0x10 + i] << (8 * i);
			if (data_size) {
				*data_size = 0;
				for (i = 0; i < 8; i++)
					*data_size |= (u64)rec[off + 0x30 + i] << (8 * i);
			}
			if (mp_ofs >= len)
				return -EINVAL;
			return decode_runs(rec + off + mp_ofs, len - mp_ofs, lo, runs, n);
		}
		off += len;
	}
	return -ENOENT;
}

static void add_runs(struct geometry *g, const struct ntfs_img_run *runs,
		     uint32_t n, int region)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (runs[i].lcn != UINT64_MAX)
			add_extent(g, runs[i].lcn * g->img.cluster_size,
				   runs[i].len * g->img.cluster_size, region);
}

static int geometry_open(struct geometry *g, const char *path)
{
	uint8_t rec[4096];
	struct ntfs_img_run *runs;
	uint32_t n;
	int err;

	memset(g, 0, sizeof(*g));
	err = ntfs_image_open(&g->img, path, false, 0, 0);
	if (err)
		return err;
	g->have = true;

	add_extent(g, 0, g->img.sector_size, R_BOOT);
	add_runs(g, g->img.mft_runs, g->img.n_mft_runs, R_MFT);
	add_runs(g, g->img.log_runs, g->img.n_log_runs, R_LOGFILE);

	if (g->img.mft_record_size <= sizeof(rec)) {
		if (read_record(g, 1, rec) == 0 &&
		    attr_runs(rec, g->img.mft_record_size, 0x80, &runs, &n, NULL) == 0) {
			add_runs(g, runs, n, R_MFTMIRR);
			free(runs);
		}
		if (read_record(g, 6, rec) == 0 &&
		    attr_runs(rec, g->img.mft_record_size, 0x80,
			      &g->bitmap_runs, &g->n_bitmap_runs, &g->bitmap_size) == 0)
			add_runs(g, g->bitmap_runs, g->n_bitmap_runs, R_BITMAP);
		/* $INDEX_ALLOCATION of the root directory: the index blocks that
		 * a create or a rename dirties. */
		if (read_record(g, 5, rec) == 0 &&
		    attr_runs(rec, g->img.mft_record_size, 0xA0, &runs, &n, NULL) == 0) {
			add_runs(g, runs, n, R_ROOTIDX);
			free(runs);
		}
	}
	return 0;
}

static void geometry_close(struct geometry *g)
{
	if (!g->have)
		return;
	free(g->bitmap_runs);
	ntfs_image_close(&g->img);
	g->have = false;
}

static int classify(const struct geometry *g, u64 off)
{
	size_t i;

	for (i = 0; i < g->n_ext; i++)
		if (off >= g->ext[i].start && off < g->ext[i].end)
			return g->ext[i].region;
	return R_OTHER;
}

/* MFT record numbers covered by [off, off+len) -- writes are page sized, so
 * one of them usually carries four records. UINT64_MAX if not in $MFT. */
static void mft_records_at(const struct geometry *g, u64 off, size_t len,
			   u64 *lo, u64 *hi)
{
	uint32_t i;

	*lo = *hi = UINT64_MAX;
	for (i = 0; i < g->img.n_mft_runs; i++) {
		const struct ntfs_img_run *r = &g->img.mft_runs[i];
		u64 s, e;

		if (r->lcn == UINT64_MAX)
			continue;
		s = r->lcn * g->img.cluster_size;
		e = s + r->len * g->img.cluster_size;
		if (off < s || off >= e)
			continue;
		{
			u64 vbo = r->vcn * g->img.cluster_size + (off - s);
			*lo = vbo / g->img.mft_record_size;
			*hi = (vbo + len - 1) / g->img.mft_record_size;
		}
		return;
	}
}

static bool cluster_is_marked_used(struct geometry *g, u64 lcn)
{
	u64 phys = ntfs_image_map(g->bitmap_runs, g->n_bitmap_runs,
				  g->img.cluster_size, lcn / 8);
	uint8_t byte;

	if (phys == UINT64_MAX || pread(g->img.fd, &byte, 1, (off_t)phys) != 1)
		return true;			/* unreadable: not our finding to make */
	return (byte & (1u << (lcn % 8))) != 0;
}

/* Runs of attribute @type in @rec, each cluster checked against $Bitmap. */
static int runs_covered(struct geometry *g, const uint8_t *rec, uint32_t type, u64 *bad_lcn)
{
	struct ntfs_img_run *runs;
	uint32_t n, i;
	int rc = 0;

	if (attr_runs(rec, g->img.mft_record_size, type, &runs, &n, NULL) != 0)
		return 0;			/* resident or absent: no clusters */
	for (i = 0; i < n && !rc; i++) {
		u64 c;

		if (runs[i].lcn == UINT64_MAX)
			continue;
		for (c = 0; c < runs[i].len; c++)
			if (!cluster_is_marked_used(g, runs[i].lcn + c)) {
				*bad_lcn = runs[i].lcn + c;
				rc = 1;
				break;
			}
	}
	free(runs);
	return rc;
}

/*
 * The unrecoverable shape, swept across the whole volume: any in-use MFT
 * record whose $DATA or $INDEX_ALLOCATION points at a cluster $Bitmap calls
 * free. The next allocation would hand that cluster to another file and
 * overwrite it, and no fsck can undo that. The opposite imbalance -- bits set
 * for clusters nobody owns -- is a leak that chkdsk reclaims, and is not
 * checked here because it is the outcome the ordering is chosen to produce.
 */
static int allocation_covered(struct geometry *g, u64 *bad_ino, u64 *bad_lcn)
{
	uint8_t rec[4096];
	u64 no, total = 0;
	uint32_t i;

	*bad_ino = *bad_lcn = 0;
	if (g->img.mft_record_size > sizeof(rec) || !g->n_bitmap_runs)
		return -EINVAL;
	for (i = 0; i < g->img.n_mft_runs; i++)
		total += g->img.mft_runs[i].len;
	total = total * g->img.cluster_size / g->img.mft_record_size;

	for (no = 0; no < total; no++) {
		if (read_record(g, no, rec) != 0)
			continue;		/* never written, or not a FILE record */
		if (!(rec[0x16] & 1))		/* MFT_RECORD_IN_USE */
			continue;
		if (runs_covered(g, rec, 0x80, bad_lcn) ||
		    runs_covered(g, rec, 0xA0, bad_lcn)) {
			*bad_ino = no;
			return 1;
		}
	}
	return 0;
}

/* $Bitmap byte offsets holding the bits for this record's $DATA clusters. */
static size_t bitmap_offsets_for(struct geometry *g, u64 mft_no, u64 *out, size_t max)
{
	uint8_t rec[4096];
	struct ntfs_img_run *runs;
	uint32_t n, i;
	size_t used = 0;

	if (g->img.mft_record_size > sizeof(rec) || read_record(g, mft_no, rec) != 0)
		return 0;
	if (attr_runs(rec, g->img.mft_record_size, 0x80, &runs, &n, NULL) != 0)
		return 0;
	for (i = 0; i < n; i++) {
		u64 c;

		if (runs[i].lcn == UINT64_MAX)
			continue;
		for (c = 0; c < runs[i].len && used < max; c++) {
			u64 phys = ntfs_image_map(g->bitmap_runs, g->n_bitmap_runs,
						  g->img.cluster_size,
						  (runs[i].lcn + c) / 8);
			size_t k;

			if (phys == UINT64_MAX)
				continue;
			for (k = 0; k < used; k++)
				if (out[k] == phys)
					break;
			if (k == used)
				out[used++] = phys;
		}
	}
	free(runs);
	return used;
}

/* ---- mounting --------------------------------------------------------- */

static struct ntfs_mount_options mopts = {
	.flags = NTFS_MOUNT_RDONLY_FALLBACK,
	.uid = 0, .gid = 0, .fmask = 022, .dmask = 022,
};

/*
 * A deliberately crashed volume makes the driver shout, and every cut point
 * produces a page of it. Swallow it unless someone is debugging.
 */
static void quiet_logger(int level, const char *msg, void *ctx)
{
	(void)ctx;
	if (getenv("NTFS_CRASH_TRACE"))
		fprintf(stderr, "  [drv %d] %s\n", level, msg);
}

/* ---- 1. the ordering invariant ---------------------------------------- */

/*
 * The design says the $Bitmap update for a file's new clusters must reach the
 * device before the MFT record that references them. Record one sync and
 * check it.
 *
 * This was confirmed to bite on 2026-09-14 by reversing both comparisons in
 * cmp_sync_order() (platform/src/inode.c), which turns "system inodes, then
 * user inodes, then inode 0" into its opposite. With the reversal the same
 * sync writes the MFT record at trace index 15 and its bitmap bits at 16, this
 * check fails, and so do the two sweeps below: the surviving volume then has
 * MFT records pointing at clusters $Bitmap calls free, which is the exact
 * corruption the ordering exists to prevent.
 */
static void test_ordering_invariant(void)
{
	char path[1024];
	struct geometry g;
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root = NULL, *f = NULL;
	char *buf;
	u64 ino = UINT64_MAX;
	u64 bm_off[64];
	size_t n_bm = 0, i, k, bitmap_for_file = 0, mft_first = 0;
	bool saw_bitmap = false, saw_mft = false;
	int err;

	scratch_path(path, sizeof(path), "order");
	if (make_image(path) != 0) {
		fprintf(stderr, "SKIP test_ordering_invariant: mkntfs unavailable\n");
		return;
	}

	dev = crash_bdev_open(path, LONG_MAX);
	CHECK(dev != NULL);
	if (!dev) { unlink(path); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK(err == 0);
	if (err) { ntfs_bdev_close(dev); unlink(path); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);

	buf = malloc(256 * 1024);
	CHECK(buf != NULL);
	if (root && buf) {
		memset(buf, 'z', 256 * 1024);
		err = ntfs_create(root, "ordering.bin", 0100644, &f);
		CHECK(err == 0);
		if (!err) {
			ino = ntfs_inode_number(f);
			CHECK(ntfs_write(f, buf, 256 * 1024, 0) == 256 * 1024);

			/* Only the sync is recorded: that is where the port
			 * chooses an order. Whatever the operation itself wrote
			 * on the way in is not a decision this design makes. */
			trace_n = 0;
			tracing = true;
			CHECK(ntfs_volume_sync(vol) == 0);
			tracing = false;
		}
	}
	if (f)
		ntfs_inode_put(f);
	if (root)
		ntfs_inode_put(root);
	free(buf);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	/*
	 * Geometry comes from the image AFTER the run, not before: this write
	 * extended $MFT, so the extents a fresh volume reports would put the new
	 * file's own MFT record in the "other" bucket and the check would pass
	 * by accident.
	 */
	if (geometry_open(&g, path) != 0) {
		FAILF("could not read geometry back from the written image\n");
		unlink(path);
		return;
	}
	if (ino != UINT64_MAX)
		n_bm = bitmap_offsets_for(&g, ino, bm_off, sizeof(bm_off) / sizeof(bm_off[0]));

	for (i = 0; i < trace_n; i++) {
		int r = classify(&g, trace[i].off);
		u64 lo, hi;

		/* Not any $Bitmap write: the one carrying the bits for THIS
		 * file's clusters. That pairing is what the design is about. */
		if (r == R_BITMAP)
			for (k = 0; k < n_bm; k++)
				if (bm_off[k] >= trace[i].off &&
				    bm_off[k] < trace[i].off + trace[i].len) {
					bitmap_for_file = i;
					saw_bitmap = true;
				}
		if (r == R_MFT && !saw_mft) {
			mft_records_at(&g, trace[i].off, trace[i].len, &lo, &hi);
			if (lo != UINT64_MAX && ino >= lo && ino <= hi) {
				mft_first = i;
				saw_mft = true;
			}
		}
	}

	if (getenv("NTFS_CRASH_TRACE")) {
		for (i = 0; i < trace_n; i++) {
			u64 lo, hi;
			mft_records_at(&g, trace[i].off, trace[i].len, &lo, &hi);
			fprintf(stderr, "  [%3zu] %-10s off=%llu len=%zu",
				i, region_name[classify(&g, trace[i].off)],
				(unsigned long long)trace[i].off, trace[i].len);
			if (lo != UINT64_MAX)
				fprintf(stderr, " mft %llu..%llu",
					(unsigned long long)lo, (unsigned long long)hi);
			fprintf(stderr, "\n");
		}
		fprintf(stderr, "  ordering.bin is inode %llu; its bits live at",
			(unsigned long long)ino);
		for (k = 0; k < n_bm; k++)
			fprintf(stderr, " %llu", (unsigned long long)bm_off[k]);
		fprintf(stderr, "\n");
	}

	checks++;
	if (!n_bm) {
		failures++;
		fprintf(stderr, "FAIL ordering: ordering.bin allocated no clusters, "
			"so there is no allocation to order\n");
	}
	checks++;
	if (!saw_mft) {
		failures++;
		fprintf(stderr, "FAIL ordering: the sync never wrote MFT record %llu, "
			"so there is nothing to order against\n", (unsigned long long)ino);
	}
	checks++;
	if (!saw_bitmap) {
		failures++;
		fprintf(stderr, "FAIL ordering: the sync never wrote the $Bitmap bytes "
			"covering ordering.bin's clusters\n");
	}
	checks++;
	if (saw_mft && saw_bitmap && bitmap_for_file > mft_first) {
		failures++;
		fprintf(stderr, "FAIL ordering: MFT record %llu reached the device at "
			"trace index %zu, its $Bitmap bits only at %zu. A crash between "
			"the two leaves a record claiming clusters the bitmap calls free, "
			"and the next allocation hands them to another file.\n",
			(unsigned long long)ino, mft_first, bitmap_for_file);
	}

	geometry_close(&g);
	unlink(path);
	printf("test_ordering_invariant (bitmap at %zu, mft record at %zu, of %zu writes)\n",
	       bitmap_for_file, mft_first, trace_n);
}

/* ---- 2. cut points through four operations ---------------------------- */

enum op_kind { OP_CREATE, OP_WRITE, OP_RENAME, OP_DELETE };

struct op {
	const char *name;
	enum op_kind kind;
	const char *base;		/* image to start from */
};

/* Perform one operation on a mounted volume. Errors are expected once the
 * device has gone dead, so they are not failures by themselves. */
static void run_op(ntfs_volume_t *vol, enum op_kind kind)
{
	ntfs_inode_t *root = NULL, *f = NULL;
	char *buf;

	if (ntfs_volume_root(vol, &root) != 0)
		return;
	switch (kind) {
	case OP_CREATE:
		if (ntfs_create(root, "new.bin", 0100644, &f) == 0) {
			char small[600];
			memset(small, 'c', sizeof(small));
			ntfs_write(f, small, sizeof(small), 0);
			ntfs_inode_put(f);
		}
		break;
	case OP_WRITE:
		if (ntfs_lookup(root, "a.bin", &f) == 0) {
			buf = malloc(128 * 1024);
			if (buf) {
				memset(buf, 'w', 128 * 1024);
				ntfs_write(f, buf, 128 * 1024, 64 * 1024);
				free(buf);
			}
			ntfs_inode_put(f);
		}
		break;
	case OP_RENAME:
		ntfs_rename(root, "a.bin", root, "renamed-to-a-longer-name.bin");
		break;
	case OP_DELETE:
		ntfs_unlink(root, "a.bin");
		break;
	}
	ntfs_volume_sync(vol);
	ntfs_inode_put(root);
}

/* Run @kind against @path with the device dying after @limit writes.
 * Returns the number of writes the operation attempted. */
static long crash_run(const char *path, enum op_kind kind, long limit)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;

	dev = crash_bdev_open(path, limit);
	if (!dev)
		return -1;
	if (ntfs_mount(dev, &mopts, &vol) != 0) {
		ntfs_bdev_close(dev);
		return -1;
	}
	run_op(vol, kind);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	return write_count;
}

/* A volume with one 64 KiB file "a.bin", clean on disk. */
static int make_base_with_file(const char *path, u64 *ino_out)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	char *buf;
	int err;

	if (make_image(path) != 0)
		return -EIO;
	dev = crash_bdev_open(path, LONG_MAX);
	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err) { ntfs_bdev_close(dev); return err; }
	err = ntfs_volume_root(vol, &root);
	if (!err) {
		err = ntfs_create(root, "a.bin", 0100644, &f);
		if (!err) {
			buf = malloc(64 * 1024);
			if (buf) {
				memset(buf, 'a', 64 * 1024);
				if (ntfs_write(f, buf, 64 * 1024, 0) != 64 * 1024)
					err = -EIO;
				free(buf);
			}
			*ino_out = ntfs_inode_number(f);
			ntfs_inode_put(f);
		}
		ntfs_inode_put(root);
	}
	ntfs_volume_sync(vol);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	return err;
}

/* Does the crashed image still mount, and is what survived self-consistent? */
struct aftermath {
	bool mounts;
	bool file_present;
	u64 size, alloc;
};

static struct aftermath remount(const char *path, const char *name)
{
	struct aftermath a = { 0 };
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;

	dev = ntfs_bdev_open_path(path, true);
	if (!dev)
		return a;
	if (ntfs_mount(dev, &mopts, &vol) != 0) {
		ntfs_bdev_close(dev);
		return a;
	}
	a.mounts = true;
	if (name && ntfs_volume_root(vol, &root) == 0) {
		if (ntfs_lookup(root, name, &f) == 0) {
			struct ntfs_attr at;

			if (ntfs_getattr(f, &at) == 0) {
				a.file_present = true;
				a.size = at.size;
				a.alloc = at.alloc_size;
			}
			ntfs_inode_put(f);
		}
		ntfs_inode_put(root);
	}
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	return a;
}

static void test_cut_points(void)
{
	static const struct op ops[] = {
		/* Create runs on the same base as the rest: it already had one
		 * file made, so $MFT is grown and the new record fits in it.
		 * test_data_before_metadata() covers the create that has to
		 * extend $MFT, which is the rarer and harder path. */
		{ "create", OP_CREATE, "file" },
		{ "write",  OP_WRITE,  "file" },
		{ "rename", OP_RENAME, "file" },
		{ "delete", OP_DELETE, "file" },
	};
	char empty[1024], withfile[1024], work[1024];
	struct geometry g;
	u64 a_ino = 0;
	size_t oi;
	int n_clean = 0, n_errors = 0, n_refused = 0, n_nomount = 0, n_uncovered = 0;
	int n_dangling = 0, n_mirror = 0, n_bitmap_leak = 0, n_unclassified = 0;

	scratch_path(empty, sizeof(empty), "base-empty");
	scratch_path(withfile, sizeof(withfile), "base-file");
	scratch_path(work, sizeof(work), "work");

	if (make_image(empty) != 0) {
		fprintf(stderr, "SKIP test_cut_points: mkntfs unavailable\n");
		return;
	}
	if (make_base_with_file(withfile, &a_ino) != 0) {
		FAILF("could not build the base image with a file\n");
		unlink(empty);
		return;
	}
	if (run_ntfsck(empty, NULL, 0) == V_SKIPPED) {
		fprintf(stderr, "SKIP test_cut_points: ntfsck unavailable at '%s'\n",
			ntfsck_path);
		unlink(empty);
		unlink(withfile);
		return;
	}
	/* The uncrashed baseline must itself be clean, or every verdict below
	 * is measuring the wrong thing. */
	checks++;
	if (run_ntfsck(withfile, NULL, 0) != V_CLEAN) {
		failures++;
		fprintf(stderr, "FAIL: the driver's own clean unmount does not pass ntfsck; "
			"the cut-point table below is not meaningful\n");
	}

	printf("\n  operation   writes  cut  verdict   mounts  ntfsck says\n");
	printf("  ---------- ------- ---- --------- -------- ------------------------------\n");

	for (oi = 0; oi < sizeof(ops) / sizeof(ops[0]); oi++) {
		const char *base = ops[oi].base ? withfile : empty;
		const char *watch = ops[oi].kind == OP_CREATE ? "new.bin" : "a.bin";
		long total, step, cut;

		if (copy_file(base, work) != 0) { FAILF("copy failed\n"); break; }
		total = crash_run(work, ops[oi].kind, LONG_MAX);
		if (total <= 0) { FAILF("%s: uncrashed run made no writes\n", ops[oi].name); continue; }

		/* Sample rather than sweep: the point is to hit every phase of the
		 * operation, and CI runs this on every push. */
		step = total / 10;
		if (step < 1)
			step = 1;
		for (cut = 1; cut <= total; cut += step) {
			char msg[256] = "";
			enum verdict v;
			struct aftermath a;
			u64 bad_lcn = 0, bad_ino = 0;
			int cov = 0;

			if (copy_file(base, work) != 0) { FAILF("copy failed\n"); break; }
			crash_run(work, ops[oi].kind, cut);

			v = run_ntfsck(work, msg, sizeof(msg));

			/* Before remounting, because a read-write mount of a
			 * crashed volume writes to it and would answer a
			 * different question than the one asked. */
			if (geometry_open(&g, work) == 0) {
				cov = allocation_covered(&g, &bad_ino, &bad_lcn);
				geometry_close(&g);
			}
			a = remount(work, watch);

			printf("  %-10s %7ld %4ld %-9s %-8s %s%s\n",
			       ops[oi].name, total, cut, verdict_name[v],
			       a.mounts ? "yes" : "NO", msg,
			       cov == 1 ? " [UNALLOCATED CLUSTERS REFERENCED]" : "");

			if (v == V_CLEAN) n_clean++;
			else if (v == V_ERRORS) n_errors++;
			else n_refused++;
			if (!a.mounts) n_nomount++;
			if (cov == 1) n_uncovered++;
			if (v == V_ERRORS) {
				if (strstr(msg, "$MFTMirr")) n_mirror++;
				else if (strstr(msg, "Failed to open inode") ||
					 strstr(msg, "open failed") ||
					 strstr(msg, "Failed to find filename")) n_dangling++;
				else if (strstr(msg, "Cluster bitmap")) n_bitmap_leak++;
				else n_unclassified++;
			}

			/* Two properties are not negotiable at any cut point:
			 * the driver can still open what it left behind, and no
			 * surviving record points at clusters the bitmap calls
			 * free. The second is the corruption the write ordering
			 * exists to prevent. */
			checks++;
			if (!a.mounts) {
				failures++;
				fprintf(stderr, "FAIL %s cut %ld: the volume no longer mounts\n",
					ops[oi].name, cut);
			}
			checks++;
			if (cov == 1) {
				failures++;
				fprintf(stderr, "FAIL %s cut %ld: inode %llu references cluster %llu, "
					"which $Bitmap marks free. The next allocation would hand "
					"it to another file.\n",
					ops[oi].name, cut, (unsigned long long)bad_ino,
					(unsigned long long)bad_lcn);
			}
			/* ntfsck refusing the volume outright is a different
			 * thing from ntfsck listing repairs: nothing can be
			 * recovered from a volume its own fsck will not read. */
			checks++;
			if (v == V_REFUSED) {
				failures++;
				fprintf(stderr, "FAIL %s cut %ld: ntfsck could not read the volume\n",
					ops[oi].name, cut);
			}
			/* Size must never exceed what is allocated for it. */
			checks++;
			if (a.file_present && a.size > a.alloc) {
				failures++;
				fprintf(stderr, "FAIL %s cut %ld: %s has size %llu but only %llu "
					"bytes allocated\n", ops[oi].name, cut, watch,
					(unsigned long long)a.size, (unsigned long long)a.alloc);
			}
		}
	}
	printf("  ---------- ------- ---- --------- -------- ------------------------------\n");
	printf("  %d clean, %d with ntfsck errors, %d refused; %d failed to mount, "
	       "%d referenced unallocated clusters\n",
	       n_clean, n_errors, n_refused, n_nomount, n_uncovered);
	printf("  error classes: %d $MFTMirr stale, %d name without a record, "
	       "%d cluster bitmap ahead of the records, %d unclassified\n",
	       n_mirror, n_dangling, n_bitmap_leak, n_unclassified);

	/*
	 * The current state, pinned rather than fixed (2026-09-14). Most cut
	 * points are NOT clean, and all three classes above are ones chkdsk or
	 * ntfsck repairs without losing data:
	 *
	 *   $MFTMirr stale               the mirror lags $MFT, rewritten from it;
	 *   name without a record        an index entry naming an MFT record that
	 *                                never landed, or a record whose $FILE_NAME
	 *                                no longer matches the index. The entry is
	 *                                removed; nothing else refers to it;
	 *   cluster bitmap ahead         bits set for clusters no record claims.
	 *                                That is the leak side of the trade, which
	 *                                is exactly what the ordering is chosen to
	 *                                produce, and the sweep above proves the
	 *                                imbalance never runs the other way.
	 *
	 * An unclassified error means a class nobody has looked at. That is worth
	 * a failure, because the whole value of this table is knowing which
	 * shapes an interrupted volume can take.
	 */
	checks++;
	if (n_unclassified) {
		failures++;
		fprintf(stderr, "FAIL: %d cut points produced an ntfsck error outside the "
			"three classes this test has examined; look at the table above\n",
			n_unclassified);
	}

	unlink(empty);
	unlink(withfile);
	unlink(work);
	printf("test_cut_points\n");
}

/* ---- 3. data before metadata ------------------------------------------ */

/*
 * The shape that must never survive a crash: a file whose size says the data
 * is there while the clusters holding it were never marked used. Walk the cut
 * points of a fresh create-and-write and check the surviving inode against
 * the surviving $Bitmap, byte for byte, at every one.
 */
static void test_data_before_metadata(void)
{
	char base[1024], work[1024];
	struct geometry g;
	long total, cut, step;
	int bad = 0;

	scratch_path(base, sizeof(base), "dbm-base");
	scratch_path(work, sizeof(work), "dbm-work");
	if (make_image(base) != 0) {
		fprintf(stderr, "SKIP test_data_before_metadata: mkntfs unavailable\n");
		return;
	}
	if (copy_file(base, work) != 0) { FAILF("copy failed\n"); unlink(base); return; }
	total = crash_run(work, OP_CREATE, LONG_MAX);
	if (total <= 0) { FAILF("create made no writes\n"); unlink(base); unlink(work); return; }

	step = total / 16;
	if (step < 1)
		step = 1;
	for (cut = 1; cut <= total; cut += step) {
		struct aftermath a;
		u64 bad_lcn = 0, bad_ino = 0;

		if (copy_file(base, work) != 0) { FAILF("copy failed\n"); break; }
		crash_run(work, OP_CREATE, cut);

		/* Sweep the surviving image before anything mounts it, so the
		 * state examined is the one the crash left. */
		checks++;
		if (geometry_open(&g, work) == 0) {
			if (allocation_covered(&g, &bad_ino, &bad_lcn) == 1) {
				failures++;
				bad++;
				fprintf(stderr, "FAIL data/metadata cut %ld: inode %llu "
					"references cluster %llu, free in $Bitmap\n",
					cut, (unsigned long long)bad_ino,
					(unsigned long long)bad_lcn);
			}
			geometry_close(&g);
		}

		a = remount(work, "new.bin");
		checks++;
		if (!a.mounts) {
			failures++;
			fprintf(stderr, "FAIL data/metadata cut %ld: volume does not mount\n", cut);
			continue;
		}
		if (!a.file_present)
			continue;		/* did not exist: the other legal outcome */

		/* The file exists, so its metadata has to be self-consistent:
		 * a size reaching past what is allocated for it is a size
		 * pointing at clusters nothing gave it. */
		checks++;
		if (a.size > a.alloc) {
			failures++;
			bad++;
			fprintf(stderr, "FAIL data/metadata cut %ld: new.bin size %llu > allocated %llu\n",
				cut, (unsigned long long)a.size, (unsigned long long)a.alloc);
		}
	}
	if (!bad)
		printf("  every cut point left new.bin either absent or fully covered "
		       "by $Bitmap\n");
	unlink(base);
	unlink(work);
	printf("test_data_before_metadata\n");
}

int main(void)
{
	tool_paths();
	ntfs_set_logger(quiet_logger, NULL);
	test_ordering_invariant();
	test_data_before_metadata();
	test_cut_points();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
