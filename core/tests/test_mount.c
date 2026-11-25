// SPDX-License-Identifier: GPL-2.0
/*
 * test_mount.c - the mount-time decisions, which had no test until 2026-09-14.
 *
 * Each case here is a bug that reached a real disk:
 *
 *   journal_clean_rule   ntfs_glue_logfile_clean() required the log to be closed
 *                        AND flagged clean. logfile.h says the two are
 *                        alternatives, and XP+ leaves the log open even across a
 *                        clean dismount, so "both" held nearly every real
 *                        Windows volume read-only (fixed 6835a53).
 *   clean_rule_agrees    ...while core/logfile's ntfs_logfile_is_clean() read
 *                        the same restart area and correctly called it clean.
 *                        Two implementations of one decision must not diverge,
 *                        so this asserts they agree on every case.
 *   nonempty_journal_rw  every fixture has an EMPTY $LogFile, so
 *                        ntfs_empty_logfile() returned at its first line and its
 *                        real path had never run under any test. The first time
 *                        it did, on a real disk, it dereferenced a NULL
 *                        bd_mapping and killed the extension (fixed 6835a53).
 *   probe_light_matches  ntfs_probe_light() was checked against ntfs_probe()
 *                        once, by hand, with a throwaway program (eea4f90).
 *                        This is that comparison, kept.
 *   one_flush_per_sync   sync_blockdev() and blkdev_issue_flush() both mapped to
 *                        a device flush, so every fsync issued two and a volume
 *                        sync three (fixed a0e6310). Nothing counted them.
 *   ro_unmount_keeps_dirty  the unmount of a read-only volume cleared
 *                        VOLUME_IS_DIRTY, on a volume marked dirty
 *                        (finding 33).
 *   error_ro_unmount     after an error took the volume read-only, the unmount
 *                        wrote everything dirty in memory and cleared the flag;
 *                        now it writes the flag alone (finding 33).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ntfscore.h"
#include "ntfsport/bdev.h"
#include "ntfs_image.h"
#include "ntfs_logfile.h"
#include "logfile_layout.h"

static int failures, checks;
#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

static const char *images_dir(void)
{
	const char *d = getenv("NTFS_IMAGES");
	return d && *d ? d : "tools/images";
}

/*
 * The log, to stderr as before, with a count of the warnings that an inode
 * outlived its unmount. Every test here puts what it got, so there must be
 * none; the teardown's eviction before put_super once logged one for each
 * inode the volume still held.
 */
static int busy_inode_logs;

static void log_to_stderr(int level, const char *msg, void *ctx)
{
	static const char *const names[] = { "err", "warn", "info", "debug" };

	(void)ctx;
	if (strstr(msg, "still referenced") || strstr(msg, "busy inodes"))
		busy_inode_logs++;
	if (level >= NTFS_LOG_ERROR && level <= NTFS_LOG_INFO)
		fprintf(stderr, "ntfs[%s]: %s\n", names[level], msg);
}

/* ---- a scratch copy of a fixture ------------------------------------- */

static char scratch[1024];

static int copy_fixture(const char *name)
{
	char src[1024];
	int in, out;
	char buf[1 << 16];
	ssize_t n;

	snprintf(src, sizeof(src), "%s/%s", images_dir(), name);
	snprintf(scratch, sizeof(scratch), "/tmp/ttntfs-test-%d-%s", (int)getpid(), name);
	in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	out = open(scratch, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) { close(in); return -1; }
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n) { close(in); close(out); return -1; }
	close(in);
	close(out);
	return 0;
}

/* ---- write a restart page pair into the scratch image's $LogFile ------ */

#define PS 4096u

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/*
 * @closed: no client in use (Win2k-style clean dismount).
 * @flags:  RESTART_VOLUME_IS_CLEAN or 0 (XP+ sets it at dismount).
 * A log that is open with the flag clear is the only dirty combination.
 */
static int write_restart_pages(const char *img_path, bool closed, uint16_t flags)
{
	struct ntfs_image img;
	uint8_t page[PS];
	uint64_t off, log_size;
	int fd, err, i;

	err = ntfs_image_open(&img, img_path, true, 0, 0);
	if (err)
		return err;
	log_size = img.log_size ? img.log_size : (1u << 20);
	fd = open(img_path, O_WRONLY);
	if (fd < 0) { ntfs_image_close(&img); return -errno; }

	for (i = 0; i < 2; i++) {
		uint8_t *ra, *cr;

		memset(page, 0, PS);
		put32(page + RP_MAGIC, LFS_MAGIC_RSTR);
		put16(page + RP_USA_OFS, RP_HEADER_SIZE);
		put16(page + RP_USA_COUNT, PS / 512 + 1);
		put32(page + RP_SYSTEM_PAGE_SIZE, PS);
		put32(page + RP_LOG_PAGE_SIZE, PS);
		put16(page + RP_RESTART_AREA_OFFSET, 0x30);
		put16(page + RP_MINOR_VER, 1);
		put16(page + RP_MAJOR_VER, 1);		/* v1.1: XP..Win7 */
		ra = page + 0x30;
		put64(ra + RA_CURRENT_LSN, 0x40000);
		put16(ra + RA_LOG_CLIENTS, 1);
		put16(ra + RA_CLIENT_FREE_LIST, closed ? 0 : LFS_NO_CLIENT);
		put16(ra + RA_CLIENT_IN_USE_LIST, closed ? LFS_NO_CLIENT : 0);
		put16(ra + RA_FLAGS, flags);
		/* check_ra() requires exactly 67 - bit_length(file_size). */
		{
			uint64_t fs = log_size;
			uint32_t bits = 0;
			while (fs) { fs >>= 1; bits++; }
			put32(ra + RA_SEQ_NUMBER_BITS, 67 - bits);
		}
		put16(ra + RA_RESTART_AREA_LENGTH, RA_SIZE_XP + CR_SIZE);
		put16(ra + RA_CLIENT_ARRAY_OFFSET, RA_SIZE_XP);
		put64(ra + RA_FILE_SIZE, log_size);
		put32(ra + RA_LAST_LSN_DATA_LENGTH, 0);
		put16(ra + RA_LOG_RECORD_HEADER_LENGTH, LR_HEADER_SIZE);
		put16(ra + RA_LOG_PAGE_DATA_OFFSET, 0x40);
		put32(ra + RA_RESTART_LOG_OPEN_COUNT, 7);
		cr = ra + RA_SIZE_XP;
		put64(cr + CR_OLDEST_LSN, 0x40000);
		put64(cr + CR_CLIENT_RESTART_LSN, 0x40000);
		put16(cr + CR_PREV_CLIENT, LFS_NO_CLIENT);
		put16(cr + CR_NEXT_CLIENT, LFS_NO_CLIENT);
		put32(cr + CR_CLIENT_NAME_LENGTH, 8);
		put16(cr + CR_CLIENT_NAME + 0, 'N');
		put16(cr + CR_CLIENT_NAME + 2, 'T');
		put16(cr + CR_CLIENT_NAME + 4, 'F');
		put16(cr + CR_CLIENT_NAME + 6, 'S');
		put16(page + RP_HEADER_SIZE, (uint16_t)(0x100 + i));
		ntfs_log_fixup_pre_write(page, PS, 512);

		off = ntfs_image_map(img.log_runs, img.n_log_runs, img.cluster_size,
				     (uint64_t)i * PS);
		if (off == UINT64_MAX) { close(fd); ntfs_image_close(&img); return -EIO; }
		if (pwrite(fd, page, PS, (off_t)off) != (ssize_t)PS) {
			close(fd); ntfs_image_close(&img); return -EIO;
		}
	}
	close(fd);
	ntfs_image_close(&img);
	return 0;
}

/* ---- mount and report the read-only decision ------------------------- */

struct mount_outcome { int err; bool read_only; int ro_reason; bool logfile_clean; };

static struct mount_outcome mount_it(const char *path)
{
	struct ntfs_mount_options o = { .flags = NTFS_MOUNT_RDONLY_FALLBACK,
					.uid = 0, .gid = 0, .fmask = 022, .dmask = 022 };
	struct mount_outcome r = { .err = 0 };
	struct ntfs_volume_info info;
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;

	dev = ntfs_bdev_open_path(path, false);
	if (!dev) { r.err = -ENOENT; return r; }
	r.err = ntfs_mount(dev, &o, &vol);
	if (r.err) { ntfs_bdev_close(dev); return r; }
	ntfs_volume_get_info(vol, &info);
	r.read_only = info.read_only;
	r.ro_reason = info.ro_reason;
	r.logfile_clean = info.logfile_clean;
	ntfs_unmount(vol);
	return r;
}

/* ---- 1. the clean rule, and the two implementations agreeing ---------- */

/*
 * A journal with no usable restart page is regenerable, not dirty -- but a v2.0
 * header the vendored checker cannot read is NOT the same thing.
 *
 * Both make ntfs_check_logfile() return false, and treating them alike mounted a
 * genuinely dirty v2.0 volume read-write during development on 2026-09-15.
 * Measured on a real Windows Recovery volume: attaching it to Windows wrote two
 * fresh v1.1 restart pages and mounted it read-write, leaving all 1113 record
 * pages alone. A missing header is a two-page fix; a header that says dirty is
 * not. This pins both.
 */
/* Erase both restart pages, leaving the record pages behind: the shape a real
 * Windows Recovery volume was found in, and what our crash left on 2026-09-13. */
static int erase_restart_pages(const char *img_path)
{
	struct ntfs_image img;
	uint8_t page[PS];
	uint64_t off;
	int fd, err, i;

	err = ntfs_image_open(&img, img_path, true, 0, 0);
	if (err)
		return err;
	fd = open(img_path, O_WRONLY);
	if (fd < 0) { ntfs_image_close(&img); return -errno; }
	memset(page, 0xff, PS);
	for (i = 0; i < 2; i++) {
		off = ntfs_image_map(img.log_runs, img.n_log_runs, img.cluster_size,
				     (uint64_t)i * PS);
		if (off == UINT64_MAX) { close(fd); ntfs_image_close(&img); return -EIO; }
		if (pwrite(fd, page, PS, (off_t)off) != (ssize_t)PS) {
			close(fd); ntfs_image_close(&img); return -EIO;
		}
	}
	/*
	 * Record pages behind the erased header, or this proves nothing: an
	 * all-0xff log is EMPTY, which ntfs_glue_logfile_clean() answers at its
	 * first line without ever reaching the branch under test. The first
	 * version of this helper omitted them and the test passed with the fix
	 * reverted -- caught only because a revert that produces no failures is
	 * treated here as suspicious rather than reassuring.
	 */
	for (i = 4; i < 8; i++) {
		memset(page, 0, PS);
		put32(page + RP_MAGIC, LFS_MAGIC_RCRD);
		put16(page + RP_USA_OFS, RP_HEADER_SIZE);
		put16(page + RP_USA_COUNT, PS / 512 + 1);
		off = ntfs_image_map(img.log_runs, img.n_log_runs, img.cluster_size,
				     (uint64_t)i * PS);
		if (off == UINT64_MAX) { close(fd); ntfs_image_close(&img); return -EIO; }
		if (pwrite(fd, page, PS, (off_t)off) != (ssize_t)PS) {
			close(fd); ntfs_image_close(&img); return -EIO;
		}
	}
	close(fd);
	ntfs_image_close(&img);
	return 0;
}

static void test_headerless_is_not_dirty(void)
{
	struct mount_outcome m;

	printf("test_headerless_is_not_dirty\n");

	/* (a) header erased, records behind it: regenerable, must go read-write. */
	if (copy_fixture("basic-4k.img") != 0) {
		fprintf(stderr, "SKIP test_headerless_is_not_dirty: no fixture\n");
		return;
	}
	if (write_restart_pages(scratch, true, RESTART_VOLUME_IS_CLEAN) == 0 &&
	    erase_restart_pages(scratch) == 0) {
		m = mount_it(scratch);
		checks++;
		if (m.err) {
			failures++;
			fprintf(stderr, "FAIL headerless: mount returned %d\n", m.err);
		} else if (m.read_only) {
			failures++;
			fprintf(stderr, "FAIL headerless: mounted READ-ONLY (reason %d); a missing "
					"restart page is regenerable, not dirty\n", m.ro_reason);
		}
	}
	unlink(scratch);

	/* (b) a header that says dirty must still be refused. */
	if (copy_fixture("basic-4k.img") != 0)
		return;
	if (write_restart_pages(scratch, false, 0) == 0) {
		m = mount_it(scratch);
		checks++;
		if (!m.err && (!m.read_only || m.ro_reason != NTFS_RO_LOGFILE)) {
			failures++;
			fprintf(stderr, "FAIL dirty: expected read-only for an unclean journal, "
					"got ro=%d reason=%d\n", (int)m.read_only, m.ro_reason);
		}
	}
	unlink(scratch);
}

static void test_journal_clean_rule(void)
{
	static const struct {
		const char *what;
		bool closed;
		uint16_t flags;
		bool expect_clean;
	} cases[] = {
		{ "closed, no flag (Win2k clean dismount)", true,  0,                       true  },
		{ "closed, flag set",                       true,  RESTART_VOLUME_IS_CLEAN, true  },
		/* The regression: XP+ leaves the log OPEN across a clean dismount and
		 * marks it with the flag. Requiring "closed AND flag" made this dirty
		 * and held nearly every real Windows volume read-only. */
		{ "open + VOLUME_IS_CLEAN (XP+ clean)",     false, RESTART_VOLUME_IS_CLEAN, true  },
		{ "open, no flag (genuinely dirty)",        false, 0,                       false },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct ntfs_logfile_analysis an;
		struct mount_outcome m;
		struct ntfs_bdev *dev;
		int err;

		if (copy_fixture("basic-4k.img") != 0) {
			fprintf(stderr, "SKIP test_journal_clean_rule: no fixture\n");
			return;
		}
		if (write_restart_pages(scratch, cases[i].closed, cases[i].flags) != 0) {
			fprintf(stderr, "FAIL: could not write restart pages for '%s'\n", cases[i].what);
			failures++; checks++;
			unlink(scratch);
			continue;
		}

		/* (a) the mount decision */
		m = mount_it(scratch);
		checks++;
		if (m.err) {
			failures++;
			fprintf(stderr, "FAIL mount '%s': %d\n", cases[i].what, m.err);
		} else if (cases[i].expect_clean) {
			if (m.read_only) {
				failures++;
				fprintf(stderr, "FAIL '%s': mounted READ-ONLY (reason %d), expected read-write\n",
					cases[i].what, m.ro_reason);
			}
		} else {
			if (!m.read_only || m.ro_reason != NTFS_RO_LOGFILE) {
				failures++;
				fprintf(stderr, "FAIL '%s': expected read-only for an unclean journal, got ro=%d reason=%d\n",
					cases[i].what, (int)m.read_only, m.ro_reason);
			}
		}

		/* (b) core/logfile must reach the same verdict on the same bytes */
		dev = ntfs_bdev_open_path(scratch, true);
		CHECK(dev != NULL);
		if (dev) {
			err = ntfs_logfile_analyse(dev, &an);
			checks++;
			if (err) {
				failures++;
				fprintf(stderr, "FAIL analyse '%s': %d\n", cases[i].what, err);
			} else if (an.clean != cases[i].expect_clean) {
				failures++;
				fprintf(stderr, "FAIL '%s': core/logfile says clean=%d, expected %d (%s)\n",
					cases[i].what, (int)an.clean, (int)cases[i].expect_clean, an.state);
			}
			/* and it must agree with the mount path */
			checks++;
			if (!m.err && an.clean != m.logfile_clean) {
				failures++;
				fprintf(stderr, "FAIL '%s': the two implementations DISAGREE "
					"(mount says clean=%d, core/logfile says %d)\n",
					cases[i].what, (int)m.logfile_clean, (int)an.clean);
			}
			ntfs_bdev_close(dev);
		}
		unlink(scratch);
	}
	printf("test_journal_clean_rule\n");
}

/* ---- 2. a read-write mount with a non-empty journal ------------------- */

static void test_nonempty_journal_rw(void)
{
	struct mount_outcome m;

	if (copy_fixture("basic-4k.img") != 0) {
		fprintf(stderr, "SKIP test_nonempty_journal_rw: no fixture\n");
		return;
	}
	/* Clean, but NOT empty: this is what makes ntfs_empty_logfile() actually
	 * run when the volume is switched read-write. With an all-0xff log it
	 * returns at its first line, which is why the crash was never seen. */
	if (write_restart_pages(scratch, false, RESTART_VOLUME_IS_CLEAN) != 0) {
		fprintf(stderr, "FAIL: could not build a non-empty journal\n");
		failures++; checks++; unlink(scratch); return;
	}
	m = mount_it(scratch);
	CHECK(m.err == 0);
	CHECK(!m.read_only);		/* must reach read-write, exercising the empty path */
	unlink(scratch);
	printf("test_nonempty_journal_rw\n");
}

/* ---- 3. probe_light must match probe -------------------------------- */

static void test_probe_light_matches(void)
{
	static const char *names[] = { "basic-4k.img", "names.img", "cluster-512.img",
				       "cluster-64k.img", "attrlist.img",
				       "compressed-sparse.img", "empty.img" };
	char path[1024];
	int any = 0;

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		struct ntfs_volume_info full, light;
		struct ntfs_bdev *dev;
		int rf, rl;

		snprintf(path, sizeof(path), "%s/%s", images_dir(), names[i]);
		if (access(path, R_OK) != 0)
			continue;
		any = 1;

		dev = ntfs_bdev_open_path(path, true);
		CHECK(dev != NULL);
		if (!dev)
			continue;
		rf = ntfs_probe(dev, &full);
		rl = ntfs_probe_light(dev, &light);
		ntfs_bdev_close(dev);

		CHECK(rf == 0);
		CHECK(rl == 0);
		if (rf || rl)
			continue;
		/* Everything the light probe claims to fill must match the full one.
		 * Free space and the hibernation/journal state are deliberately not
		 * filled by the light probe and are not compared. */
		checks++;
		if (strcmp(full.label, light.label) != 0) {
			failures++;
			fprintf(stderr, "FAIL %s: label '%s' vs light '%s'\n",
				names[i], full.label, light.label);
		}
		CHECK(full.serial == light.serial);
		CHECK(full.major_ver == light.major_ver);
		CHECK(full.minor_ver == light.minor_ver);
		CHECK(full.cluster_size == light.cluster_size);
		CHECK(full.sector_size == light.sector_size);
		CHECK(full.mft_record_size == light.mft_record_size);
		CHECK(full.total_clusters == light.total_clusters);
		CHECK(full.dirty == light.dirty);
	}
	if (!any)
		fprintf(stderr, "SKIP test_probe_light_matches: no fixtures\n");
	printf("test_probe_light_matches\n");
}

/* ---- 4. exactly one device flush per sync ---------------------------- */

static int flush_count;
static unsigned long write_count;
static const struct ntfs_bdev_ops *real_ops;
static struct ntfs_bdev_ops counting_ops;

/*
 * Two more things the wrapper can do, both off unless a case sets them: keep
 * where each write landed (the first MAX_KEPT of them), and refuse the next
 * fail_times writes that touch [fail_lo, fail_hi).
 */
#define MAX_KEPT 16
static bool keep_writes;
static unsigned kept;
static uint64_t kept_off[MAX_KEPT], kept_len[MAX_KEPT];
static uint64_t fail_lo, fail_hi;
static int fail_times, failed_writes;

static ssize_t c_pread(struct ntfs_bdev *d, void *b, size_t c, u64 o) { return real_ops->pread(d, b, c, o); }
static ssize_t c_pwrite(struct ntfs_bdev *d, const void *b, size_t c, u64 o)
{
	write_count++;
	if (fail_times && o < fail_hi && o + c > fail_lo) {
		fail_times--;
		failed_writes++;
		return -EIO;
	}
	if (keep_writes && kept < MAX_KEPT) {
		kept_off[kept] = o;
		kept_len[kept] = c;
		kept++;
	}
	return real_ops->pwrite(d, b, c, o);
}
static int c_flush(struct ntfs_bdev *d) { flush_count++; return real_ops->flush(d); }
static void c_close(struct ntfs_bdev *d) { real_ops->close(d); }

/*
 * Retiring a clean journal must cost two pages, not the whole log.
 *
 * ntfs_empty_logfile() -- upstream's, and what ntfs-3g's "recover" does -- walks
 * every page from VCN 0 writing 0xff, and destroys the two restart pages FIRST.
 * Measured at the device on a real Windows volume, a read-write mount that
 * changed nothing else cost 1420 writes and 5.5 MiB; a 64 MiB journal makes that
 * 16384 pages. A crash inside that loop leaves a journal full of recoverable
 * work with nothing left to reach it by, which is what this driver did to a
 * user's disk on 2026-09-13.
 *
 * ntfs_logfile_mark_clean_device() writes two restart pages, last, and leaves
 * what Windows leaves on a clean dismount -- verified field by field against a
 * real Windows safe-removal (E1). This pins the cost, because a regression here
 * is silent: the volume still mounts, it just writes thousands of pages again.
 */
static void test_rw_mount_retires_the_journal_in_two_writes(void)
{
	struct ntfs_mount_options o = { .flags = 0, .uid = 0, .gid = 0, .fmask = 022, .dmask = 022 };
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	int err;

	printf("test_rw_mount_retires_the_journal_in_two_writes\n");

	if (copy_fixture("basic-4k.img") != 0) {
		fprintf(stderr, "SKIP test_rw_mount_retires_the_journal_in_two_writes: no fixture\n");
		return;
	}
	/* Clean but NOT empty: an empty journal is skipped at the first line and
	 * this would measure nothing. erase_restart_pages() writes record pages
	 * for exactly this reason, so reuse its second half by writing a valid
	 * header over the top of them. */
	if (erase_restart_pages(scratch) != 0 ||
	    write_restart_pages(scratch, true, RESTART_VOLUME_IS_CLEAN) != 0) {
		fprintf(stderr, "SKIP: could not build a non-empty clean journal\n");
		unlink(scratch);
		return;
	}

	dev = ntfs_bdev_open_path(scratch, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(scratch); return; }
	real_ops = dev->ops;
	counting_ops = *real_ops;
	counting_ops.pread = c_pread; counting_ops.pwrite = c_pwrite;
	counting_ops.flush = c_flush; counting_ops.close = c_close;
	dev->ops = &counting_ops;

	write_count = 0;
	err = ntfs_mount(dev, &o, &vol);
	CHECK(err == 0);
	if (!err) {
		checks++;
		if (write_count > 8) {
			failures++;
			fprintf(stderr, "FAIL: retiring a clean journal took %lu device writes. "
					"Two restart pages is the whole job; the full erase is "
					"thousands, and destroys them first.\n", write_count);
		}
		ntfs_unmount(vol);
	}
	unlink(scratch);
}


static void test_one_flush_per_sync(void)
{
	struct ntfs_mount_options o = { .flags = 0, .uid = 0, .gid = 0, .fmask = 022, .dmask = 022 };
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	char buf[4096];
	int err;

	if (copy_fixture("basic-4k.img") != 0) {
		fprintf(stderr, "SKIP test_one_flush_per_sync: no fixture\n");
		return;
	}
	dev = ntfs_bdev_open_path(scratch, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(scratch); return; }
	real_ops = dev->ops;
	counting_ops = *real_ops;
	counting_ops.pread = c_pread; counting_ops.pwrite = c_pwrite;
	counting_ops.flush = c_flush; counting_ops.close = c_close;
	dev->ops = &counting_ops;

	err = ntfs_mount(dev, &o, &vol);
	CHECK(err == 0);
	if (err) { unlink(scratch); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);

	memset(buf, 'x', sizeof(buf));
	err = ntfs_create(root, "flushtest.bin", 0100644, &f);
	CHECK(err == 0);
	if (!err) {
		CHECK(ntfs_write(f, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));

		/*
		 * One volume sync must cost one device flush. It used to cost
		 * three: sync_blockdev() and blkdev_issue_flush() both mapped to
		 * ntfs_bdev_flush(), and sync_filesystem() added a third of its
		 * own. sync_blockdev() is page-cache writeback in Linux and
		 * issues no barrier.
		 */
		flush_count = 0;
		CHECK(ntfs_volume_sync(vol) == 0);
		checks++;
		if (flush_count != 1) {
			failures++;
			fprintf(stderr, "FAIL: ntfs_volume_sync issued %d device flushes, expected 1\n",
				flush_count);
		}

		/* And a second sync with nothing dirty still issues exactly one. */
		flush_count = 0;
		CHECK(ntfs_volume_sync(vol) == 0);
		checks++;
		if (flush_count != 1) {
			failures++;
			fprintf(stderr, "FAIL: idle ntfs_volume_sync issued %d flushes, expected 1\n",
				flush_count);
		}
		ntfs_inode_put(f);
	}
	ntfs_inode_put(root);
	ntfs_unmount(vol);
	unlink(scratch);
	printf("test_one_flush_per_sync\n");
}

/* ---- 5. unmounting a read-only volume writes nothing ------------------ */

/*
 * Finding 33 in docs/UPSTREAM-BUGS.md. ntfs_unmount() skips its sync on a
 * read-only volume, but the teardown after it ran the platform's
 * sync_filesystem(), which, unlike Linux's, does not return early for a
 * read-only super block. ntfs_sync_fs() cleared VOLUME_IS_DIRTY, and every
 * dirty inode was written.
 *
 *   ro_unmount_keeps_dirty  A volume marked dirty is mounted read-only for
 *                        that very reason ("run chkdsk in Windows"). Its
 *                        unmount wrote $Volume's record and its $MFTMirr copy
 *                        with the flag cleared, so Windows would not run
 *                        chkdsk. Measured on empty.img: 2 writes, flags 0x0001
 *                        to 0x0000. The same with a read-only mount by request,
 *                        with a read-write mount the flag refused, and with
 *                        ntfs_probe(), which the extension's check runs.
 *   error_ro_unmount     A read-write volume that an error took read-only had
 *                        every dirty inode written at unmount, whatever the
 *                        failed operation had left half done, and the flag
 *                        cleared. Now the unmount's one write is the flag, set,
 *                        in $Volume's record and its copy, so that chkdsk runs.
 *   errors_rw_unmount    U23. An error that leaves the volume writable: the MFT
 *                        allocator offers a record that $MFT's bitmap calls
 *                        free, finds it in use, warns "Unmount and run chkdsk",
 *                        sets NVolErrors() and takes another. ntfs_sync_fs()
 *                        cleared VOLUME_IS_DIRTY all the same, at every sync
 *                        and at unmount, so chkdsk was never asked for.
 *
 * error_ro_unmount also unmounts with dirty inodes that nothing holds. The
 * teardown left them to sb_free(), after put_super had freed the volume, and
 * evicting a dirty inode writes it through ni->vol. The test runs with
 * MallocScribble set (CMakeLists.txt), so that a freed volume reads as 0x55
 * bytes and a use of it crashes.
 */
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Where $Volume's record (MFT record 3) lies on the image, in $MFT and in
 * $MFTMirr, its size, and the byte offset of the volume flags in each copy. */
struct vol_rec {
	uint64_t rec[2], flags[2];
	uint32_t size;
};

static long find_vol_flags(int fd, uint64_t rec_off, uint32_t size)
{
	uint8_t r[4096];
	uint32_t a;

	if (size > sizeof(r) || pread(fd, r, size, (off_t)rec_off) != (ssize_t)size ||
	    memcmp(r, "FILE", 4))
		return -1;
	for (a = get16(r + 0x14); a + 0x18 <= size; a += get32(r + a + 4)) {
		uint32_t type = get32(r + a);

		if (type == 0xffffffffu || get32(r + a + 4) < 0x18)
			return -1;
		if (type == 0x70)	/* $VOLUME_INFORMATION; flags at +0x0a */
			return (long)(rec_off + a + get16(r + a + 0x14) + 0x0a);
	}
	return -1;
}

static int locate_vol_rec(const char *path, struct vol_rec *v)
{
	uint8_t bs[512];
	uint64_t cs;
	int8_t c;
	int fd, i;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (pread(fd, bs, sizeof(bs), 0) != (ssize_t)sizeof(bs)) {
		close(fd);
		return -1;
	}
	cs = (uint64_t)get16(bs + 0x0b) * bs[0x0d];
	c = (int8_t)bs[0x40];
	v->size = c > 0 ? (uint32_t)(c * cs) : 1u << -c;
	v->rec[0] = get64(bs + 0x30) * cs + 3 * (uint64_t)v->size;
	v->rec[1] = get64(bs + 0x38) * cs + 3 * (uint64_t)v->size;
	for (i = 0; i < 2; i++) {
		long o = find_vol_flags(fd, v->rec[i], v->size);

		if (o < 0) {
			close(fd);
			return -1;
		}
		v->flags[i] = (uint64_t)o;
	}
	close(fd);
	return 0;
}

/* The volume flags as the image holds them, in $MFT's copy; -1 on error. */
static int vol_flags_on_disk(const char *path, const struct vol_rec *v, int copy)
{
	uint8_t f[2];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = pread(fd, f, 2, (off_t)v->flags[copy]);
	close(fd);
	return n == 2 ? get16(f) : -1;
}

/* Set VOLUME_IS_DIRTY in both copies, as a session that did not finish
 * leaves it, or Windows when it flags the disk for chkdsk. */
static int mark_dirty_on_disk(const char *path, const struct vol_rec *v)
{
	int fd = open(path, O_RDWR), i;

	if (fd < 0)
		return -1;
	for (i = 0; i < 2; i++) {
		uint8_t f[2];

		if (pread(fd, f, 2, (off_t)v->flags[i]) != 2) {
			close(fd);
			return -1;
		}
		put16(f, get16(f) | 0x0001);
		if (pwrite(fd, f, 2, (off_t)v->flags[i]) != 2) {
			close(fd);
			return -1;
		}
	}
	close(fd);
	return 0;
}

/* ntfsck -n on @path, if $NTFSCK is usable. */
static void fsck_image(const char *path, const char *what)
{
	const char *ck = getenv("NTFSCK");
	char cmd[2048];
	int rc;

	if (!ck || !*ck || access(ck, X_OK) != 0) {
		fprintf(stderr, "SKIP ntfsck after %s: $NTFSCK not usable\n", what);
		return;
	}
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' >/dev/null 2>&1", ck, path);
	rc = system(cmd);
	checks++;
	if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
		failures++;
		fprintf(stderr, "FAIL ntfsck -n after %s: exit %d\n", what,
			WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
	}
}

static struct ntfs_bdev *open_counting(const char *path)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);

	if (!dev)
		return NULL;
	real_ops = dev->ops;
	counting_ops = *real_ops;
	counting_ops.pread = c_pread; counting_ops.pwrite = c_pwrite;
	counting_ops.flush = c_flush; counting_ops.close = c_close;
	dev->ops = &counting_ops;
	return dev;
}

/*
 * One way of opening a volume marked dirty: a mount with @flags,
 * which must return @want_err and, when it mounts, be read-only for
 * @want_reason; or, with @probe, ntfs_probe(), which mounts read-only by
 * request and unmounts. None of it may write to the device.
 */
struct dirty_case {
	const char *what;
	bool probe;
	uint32_t flags;
	int want_err, want_reason;
};

static void ro_unmount_case(const struct dirty_case *c)
{
	struct ntfs_mount_options o = { .flags = c->flags, .uid = 0, .gid = 0,
					.fmask = 022, .dmask = 022 };
	const char *what = c->what;
	struct ntfs_volume_info info;
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	struct vol_rec v;
	int err, fl;

	if (copy_fixture("empty.img") != 0) {
		fprintf(stderr, "SKIP ro_unmount_keeps_dirty: no fixture\n");
		return;
	}
	err = locate_vol_rec(scratch, &v);
	CHECK(err == 0);
	if (err || mark_dirty_on_disk(scratch, &v) != 0) {
		CHECK(!"mark the fixture dirty");
		unlink(scratch);
		return;
	}
	dev = open_counting(scratch);
	CHECK(dev != NULL);
	if (!dev) { unlink(scratch); return; }
	write_count = 0;
	if (c->probe) {
		memset(&info, 0, sizeof(info));
		CHECK(ntfs_probe(dev, &info) == 0);
		CHECK(info.dirty);
	} else {
		err = ntfs_mount(dev, &o, &vol);
		CHECK(err == c->want_err);
		if (!err) {
			CHECK(ntfs_volume_get_info(vol, &info) == 0);
			CHECK(info.read_only);
			CHECK(info.ro_reason == c->want_reason);
			ntfs_unmount(vol);
		}
	}
	ntfs_bdev_close(dev);
	checks++;
	if (write_count) {
		failures++;
		fprintf(stderr, "FAIL ro_unmount_keeps_dirty (%s): a volume that was never "
			"writable got %lu device writes\n", what, write_count);
	}
	for (int i = 0; i < 2; i++) {
		fl = vol_flags_on_disk(scratch, &v, i);
		checks++;
		if (fl < 0 || !(fl & 0x0001)) {
			failures++;
			fprintf(stderr, "FAIL ro_unmount_keeps_dirty (%s): VOLUME_IS_DIRTY is "
				"gone from %s afterwards (flags %#x)\n", what,
				i ? "$MFTMirr" : "$MFT", fl);
		}
	}
	fsck_image(scratch, what);
	unlink(scratch);
}

static void test_ro_unmount_keeps_dirty(void)
{
	static const struct dirty_case cases[] = {
		{ "read-only fallback", false, NTFS_MOUNT_RDONLY_FALLBACK, 0, NTFS_RO_DIRTY },
		{ "read-only by request", false, NTFS_MOUNT_RDONLY, 0, NTFS_RO_REQUESTED },
		{ "read-write mount refused", false, 0, -EROFS, 0 },
		{ "ntfs_probe", true, 0, 0, 0 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		ro_unmount_case(&cases[i]);
	printf("test_ro_unmount_keeps_dirty\n");
}

/* Does [off, off + len) lie inside one of @v's two records? */
static bool in_vol_rec(const struct vol_rec *v, uint64_t off, uint64_t len)
{
	for (int i = 0; i < 2; i++)
		if (off >= v->rec[i] && off + len <= v->rec[i] + v->size)
			return true;
	return false;
}

/* Bytes that differ between @a and @b outside @v's two records; -1 on error. */
static long long diff_outside(const char *a, const char *b, const struct vol_rec *v)
{
	static uint8_t x[1 << 16], y[1 << 16];
	long long n = 0;
	uint64_t off = 0;
	int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
	ssize_t ra, rb;

	if (fa < 0 || fb < 0) {
		if (fa >= 0) close(fa);
		if (fb >= 0) close(fb);
		return -1;
	}
	while ((ra = read(fa, x, sizeof(x))) > 0) {
		rb = read(fb, y, (size_t)ra);
		if (rb != ra) { n = -1; break; }
		for (ssize_t i = 0; i < ra; i++)
			if (x[i] != y[i] && !in_vol_rec(v, off + (uint64_t)i, 1))
				n++;
		off += (uint64_t)ra;
	}
	close(fa);
	close(fb);
	return n;
}

static int copy_file(const char *from, const char *to)
{
	static char buf[1 << 16];
	int in = open(from, O_RDONLY), out;
	ssize_t n;

	if (in < 0)
		return -1;
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) { close(in); return -1; }
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n) { n = -1; break; }
	close(in);
	close(out);
	return n < 0 ? -1 : 0;
}

static void test_error_ro_unmount(void)
{
	struct ntfs_mount_options o = { .flags = 0, .uid = 0, .gid = 0, .fmask = 022, .dmask = 022 };
	static uint8_t buf[65536];
	struct ntfs_volume_info info;
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *a = NULL, *b = NULL;
	struct vol_rec v;
	char snap[1100];
	long long diff;
	int err, fl, fs_err;
	unsigned i, in_rec = 0;

	printf("test_error_ro_unmount\n");
	if (copy_fixture("empty.img") != 0) {
		fprintf(stderr, "SKIP test_error_ro_unmount: no fixture\n");
		return;
	}
	snprintf(snap, sizeof(snap), "%s.after-error", scratch);
	err = locate_vol_rec(scratch, &v);
	CHECK(err == 0);
	dev = err ? NULL : open_counting(scratch);
	CHECK(dev != NULL);
	if (!dev) { unlink(scratch); return; }
	err = ntfs_mount(dev, &o, &vol);
	CHECK(err == 0);
	if (err) { ntfs_bdev_close(dev); unlink(scratch); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);

	/* A file, synced: the volume is clean on the disk again after this. */
	memset(buf, 'a', sizeof(buf));
	CHECK(ntfs_create(root, "a.bin", 0100644, &a) == 0);
	if (!a) goto out;
	CHECK(ntfs_write(a, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
	CHECK(ntfs_fsync(a, false) == 0);
	CHECK(ntfs_volume_sync(vol) == 0);
	fl = vol_flags_on_disk(scratch, &v, 0);
	CHECK(fl >= 0 && !(fl & 0x0001));

	/* Then changes nobody syncs: more of a.bin, and a second file that
	 * nothing will hold when the volume is unmounted. */
	memset(buf, 'b', sizeof(buf));
	CHECK(ntfs_write(a, buf, sizeof(buf), sizeof(buf)) == (ssize_t)sizeof(buf));
	CHECK(ntfs_create(root, "b.bin", 0100644, &b) == 0);
	if (b) {
		CHECK(ntfs_write(b, buf, 4096, 0) == 4096);
		ntfs_inode_put(b);
		b = NULL;
	}

	/* The error: the next write into $MFT is refused, which is a.bin's
	 * record when it is synced, and ntfs_error() takes the volume read-only. */
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	fail_lo = v.rec[0] - 3 * (uint64_t)v.size;
	fail_hi = fail_lo + info.total_mft_records * (uint64_t)v.size;
	failed_writes = 0;
	fail_times = 1;
	fs_err = ntfs_fsync(a, false);
	fail_times = 0;
	printf("  error_ro_unmount: ntfs_fsync %d, %d write(s) refused\n", fs_err, failed_writes);
	CHECK(failed_writes == 1);
	CHECK(fs_err != 0);
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	CHECK(info.read_only);
	CHECK(info.ro_reason == NTFS_RO_ERRORS);
	ntfs_inode_put(a);
	a = NULL;
	ntfs_inode_put(root);
	root = NULL;

	/* What the error left on the disk, to compare the unmount with. */
	CHECK(copy_file(scratch, snap) == 0);
	kept = 0;
	write_count = 0;
	keep_writes = true;
	ntfs_unmount(vol);
	vol = NULL;
	keep_writes = false;
	ntfs_bdev_close(dev);
	dev = NULL;

	for (i = 0; i < kept; i++)
		in_rec += in_vol_rec(&v, kept_off[i], kept_len[i]);
	printf("  error_ro_unmount: the unmount made %lu device writes, %u of them into "
	       "$Volume's record\n", write_count, in_rec);
	checks++;
	if (write_count != 2 || in_rec != 2) {
		failures++;
		fprintf(stderr, "FAIL error_ro_unmount: the unmount made %lu device writes, %u of "
			"them into $Volume's record and its copy; expected those two alone\n",
			write_count, in_rec);
	}
	diff = diff_outside(scratch, snap, &v);
	checks++;
	if (diff != 0) {
		failures++;
		fprintf(stderr, "FAIL error_ro_unmount: the unmount changed %lld bytes of the "
			"image outside $Volume's record\n", diff);
	}
	for (int c = 0; c < 2; c++) {
		fl = vol_flags_on_disk(scratch, &v, c);
		checks++;
		if (fl < 0 || !(fl & 0x0001)) {
			failures++;
			fprintf(stderr, "FAIL error_ro_unmount: VOLUME_IS_DIRTY is not set in %s "
				"after the unmount (flags %#x)\n", c ? "$MFTMirr" : "$MFT", fl);
		}
	}
	/*
	 * Not ntfsck -n here: it would judge what the fsync left, which the
	 * comparison above shows the unmount did not touch. The device refused
	 * a.bin's record, and the fsync went on after the error: 8 more writes,
	 * root's index among them. That index names b.bin, whose record was
	 * never written, and ntfsck reports the entry corrupted. The unmount used
	 * to complete that by writing everything, then marked the volume clean;
	 * now it leaves it dirty, so that chkdsk repairs it.
	 */
out:
	if (a) ntfs_inode_put(a);
	if (root) ntfs_inode_put(root);
	if (vol) ntfs_unmount(vol);
	if (dev) ntfs_bdev_close(dev);
	unlink(scratch);
	unlink(snap);
}

/*
 * The byte of $MFT's bitmap on the image that holds the bit of MFT record
 * @rec, or -1. The bitmap is record 0's $BITMAP attribute, non-resident; one
 * run covers every record of a small volume.
 */
static long long mft_bitmap_byte(const char *path, uint64_t rec)
{
	uint8_t bs[512], r[4096];
	uint64_t cs, len = 0;
	uint32_t size, a;
	int64_t lcn = 0;
	long long ret = -1;
	int8_t c;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;
	if (pread(fd, bs, sizeof(bs), 0) != (ssize_t)sizeof(bs))
		goto out;
	cs = (uint64_t)get16(bs + 0x0b) * bs[0x0d];
	c = (int8_t)bs[0x40];
	size = c > 0 ? (uint32_t)(c * cs) : 1u << -c;
	if (size > sizeof(r) ||
	    pread(fd, r, size, (off_t)(get64(bs + 0x30) * cs)) != (ssize_t)size ||
	    memcmp(r, "FILE", 4))
		goto out;
	/* Undo the fixups: the update sequence holds the last two bytes of
	 * every 512-byte block. */
	for (uint32_t i = 1; i < get16(r + 6) && i * 512u <= size; i++)
		memcpy(r + i * 512u - 2, r + get16(r + 4) + 2u * i, 2);
	for (a = get16(r + 0x14); a + 0x48 <= size; a += get32(r + a + 4)) {
		const uint8_t *p;
		int ls, os, i;

		if (get32(r + a) == 0xffffffffu || get32(r + a + 4) < 0x18)
			break;
		if (get32(r + a) != 0xb0)
			continue;
		if (!r[a + 8])			/* resident */
			break;
		p = r + a + get16(r + a + 0x20);	/* the first mapping pair */
		ls = p[0] & 0x0f;
		os = p[0] >> 4;
		if (p + 1 + ls + os > r + size || !ls || !os || os > 7)
			break;
		for (i = ls; i > 0; i--)
			len = len << 8 | p[i];
		for (i = ls + os; i > ls; i--)
			lcn = lcn << 8 | p[i];
		if (rec / 8 < len * cs)
			ret = (long long)((uint64_t)lcn * cs + rec / 8);
		break;
	}
out:
	close(fd);
	return ret;
}

static void test_errors_rw_unmount(void)
{
	struct ntfs_mount_options o = { .flags = 0, .uid = 0, .gid = 0, .fmask = 022, .dmask = 022 };
	struct ntfs_volume_info info;
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *f = NULL;
	struct vol_rec v;
	uint64_t x_no = 0, y_no = 0;
	long long at = -1;
	uint8_t byte = 0, bit;
	int err, fl, fd;

	printf("test_errors_rw_unmount\n");
	if (copy_fixture("empty.img") != 0) {
		fprintf(stderr, "SKIP test_errors_rw_unmount: no fixture\n");
		return;
	}
	err = locate_vol_rec(scratch, &v);
	CHECK(err == 0);
	if (err)
		goto out;

	/* x.bin, in the first record the MFT allocator gives out. */
	dev = ntfs_bdev_open_path(scratch, false);
	CHECK(dev != NULL);
	if (!dev)
		goto out;
	err = ntfs_mount(dev, &o, &vol);
	CHECK(err == 0);
	if (err)
		goto out;
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_create(root, "x.bin", 0100644, &f) == 0);
	if (f) {
		x_no = ntfs_inode_number(f);
		ntfs_inode_put(f);
		f = NULL;
	}
	ntfs_inode_put(root);
	root = NULL;
	CHECK(ntfs_unmount(vol) == 0);
	vol = NULL;
	ntfs_bdev_close(dev);
	dev = NULL;

	/* Its bit cleared in $MFT's bitmap: free there, in use in the record. */
	if (x_no)
		at = mft_bitmap_byte(scratch, x_no);
	CHECK(at >= 0);
	if (at < 0)
		goto out;
	bit = (uint8_t)(1u << (x_no % 8));
	fd = open(scratch, O_RDWR);
	CHECK(fd >= 0);
	if (fd < 0)
		goto out;
	CHECK(pread(fd, &byte, 1, (off_t)at) == 1 && (byte & bit));
	byte &= (uint8_t)~bit;
	CHECK(pwrite(fd, &byte, 1, (off_t)at) == 1);
	close(fd);

	/*
	 * y.bin: the allocator offers x.bin's record again, finds it in use, and
	 * takes another one. That error does not call ntfs_error(), so the
	 * volume stays writable.
	 */
	dev = ntfs_bdev_open_path(scratch, false);
	CHECK(dev != NULL);
	if (!dev)
		goto out;
	err = ntfs_mount(dev, &o, &vol);
	CHECK(err == 0);
	if (err)
		goto out;
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_create(root, "y.bin", 0100644, &f) == 0);
	if (f) {
		y_no = ntfs_inode_number(f);
		ntfs_inode_put(f);
		f = NULL;
	}
	printf("  errors_rw_unmount: x.bin in record %" PRIu64 ", y.bin in record %" PRIu64 "\n",
	       x_no, y_no);
	CHECK(y_no != 0 && y_no != x_no);
	CHECK(ntfs_lookup(root, "x.bin", &f) == 0);
	if (f) {
		CHECK(ntfs_inode_number(f) == x_no);
		ntfs_inode_put(f);
		f = NULL;
	}
	ntfs_inode_put(root);
	root = NULL;
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	CHECK(!info.read_only);

	/* The volume has errors: neither a sync nor the unmount marks it clean. */
	CHECK(ntfs_volume_sync(vol) == 0);
	fl = vol_flags_on_disk(scratch, &v, 0);
	checks++;
	if (fl < 0 || !(fl & 0x0001)) {
		failures++;
		fprintf(stderr, "FAIL errors_rw_unmount: a sync after the error cleared "
			"VOLUME_IS_DIRTY (flags %#x)\n", fl);
	}
	ntfs_unmount(vol);
	vol = NULL;
	ntfs_bdev_close(dev);
	dev = NULL;
	for (int i = 0; i < 2; i++) {
		fl = vol_flags_on_disk(scratch, &v, i);
		checks++;
		if (fl < 0 || !(fl & 0x0001)) {
			failures++;
			fprintf(stderr, "FAIL errors_rw_unmount: VOLUME_IS_DIRTY is not set in %s "
				"after the unmount (flags %#x)\n", i ? "$MFTMirr" : "$MFT", fl);
		}
	}
	/* The allocator set the bit again, so the flag is all ntfsck finds. */
	fsck_image(scratch, "errors_rw_unmount");
out:
	if (root) ntfs_inode_put(root);
	if (vol) ntfs_unmount(vol);
	if (dev) ntfs_bdev_close(dev);
	unlink(scratch);
}

int main(void)
{
	ntfs_set_logger(log_to_stderr, NULL);
	test_journal_clean_rule();
	test_headerless_is_not_dirty();
	test_rw_mount_retires_the_journal_in_two_writes();
	test_nonempty_journal_rw();
	test_probe_light_matches();
	test_one_flush_per_sync();
	test_ro_unmount_keeps_dirty();
	test_error_ro_unmount();
	test_errors_rw_unmount();
	CHECK(busy_inode_logs == 0);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
