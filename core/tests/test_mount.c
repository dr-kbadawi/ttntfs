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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

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

static ssize_t c_pread(struct ntfs_bdev *d, void *b, size_t c, u64 o) { return real_ops->pread(d, b, c, o); }
static ssize_t c_pwrite(struct ntfs_bdev *d, const void *b, size_t c, u64 o) { write_count++; return real_ops->pwrite(d, b, c, o); }
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

int main(void)
{
	test_journal_clean_rule();
	test_headerless_is_not_dirty();
	test_rw_mount_retires_the_journal_in_two_writes();
	test_nonempty_journal_rw();
	test_probe_light_matches();
	test_one_flush_per_sync();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
