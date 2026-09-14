// SPDX-License-Identifier: GPL-2.0
/*
 * test_faults.c - what the driver does when the device says no.
 *
 * Until this file there was no fault injection anywhere in the project: every
 * test ran against a device that never failed, so every error path in the port
 * was dead code that had never executed. For a filesystem that is the wrong
 * half to leave untested. A read error that is dropped shows the user stale or
 * zeroed data; a write error that is dropped is silent corruption, because the
 * caller committed and the bytes never landed.
 *
 * The mechanism is the ntfs_bdev vtable: we open a real image, keep its ops,
 * and substitute our own that fail on demand (test_mount.c counts flushes the
 * same way). Faults are selected by byte range so a case can name exactly what
 * it is breaking -- the boot sector, the first $MFT record, one file's data --
 * and by sequence number so a fault can start partway through a multi-block
 * transfer rather than only on its first block.
 *
 * Every case ends the same way: unmount, then ntfsck -n on the image, exit 0
 * required. That is the assertion that matters. Reporting -EIO correctly and
 * leaving a torn index behind is still a lost filesystem, and only the checker
 * can tell the two apart. -n is not optional: ntfsck would otherwise repair the
 * damage it is being asked to measure.
 *
 * What this found, all three fixed on 2026-09-14 and now asserted the right way
 * round (docs/UPSTREAM-BUGS.md findings 12-14):
 *
 *   A failed device flush was dropped: ntfs_fsync() and ntfs_volume_sync() both
 *   returned 0 after ntfs_bdev_flush() returned -EIO, so fsync() lied about
 *   durability. Both call sites now keep the return value. See test_flush_fail.
 *
 *   A volume forced read-only by errors reported ro_reason NTFS_RO_NONE, so the
 *   user was told their disk had gone read-only for no stated reason. See
 *   test_write_fail_data.
 *
 *   A read error at mount was reported as -EINVAL, the same errno as a
 *   partition that is not NTFS at all. See test_read_fail_mount, and
 *   test_not_ntfs_is_einval for the direction that must not move.
 *
 * Everything else the port gets right, and the assertions below say so: read
 * errors reach ntfs_read(), write errors reach ntfs_write()/ntfs_fsync()/
 * ntfs_volume_sync(), the volume drops to read-only, and every case here leaves
 * an image ntfsck calls clean.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>

#include "ntfscore.h"
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

/* A fresh mkntfs volume of @mb megabytes. Small on purpose: ENOSPC has to be
 * reachable in under a second, and every case wants its own image. */
static int make_image(const char *tag, unsigned mb)
{
	char cmd[2048];

	snprintf(img, sizeof(img), "/tmp/ttntfs-faults-%d-%s.img", (int)getpid(), tag);
	snprintf(cmd, sizeof(cmd),
		 "dd if=/dev/zero of='%s' bs=1m count=%u >/dev/null 2>&1 && "
		 "'%s' -Q -F -L FAULT '%s' >/dev/null 2>&1",
		 img, mb, tool("NTFS_MKNTFS", "tools/.local/sbin/mkntfs"), img);
	if (system(cmd) != 0) {
		fprintf(stderr, "SKIP %s: could not build an image (mkntfs missing?)\n", tag);
		img[0] = 0;
		return -1;
	}
	return 0;
}

/*
 * The assertion the whole file exists for. Never without -n: ntfsck would
 * repair the image and we would be measuring the repair, not the driver.
 *
 * Its sensitivity was measured rather than assumed, by damaging a known-good
 * image by hand: a dangling index entry (an MFT record zeroed under a live
 * name) gives exit 4, and a $Bitmap that claims allocated clusters are free
 * gives exit 4. Those are the two shapes a torn metadata write leaves behind,
 * so this check can fail. It is not all-seeing -- a zeroed MFT record with no
 * index entry pointing at it still reports clean -- which is a reason to keep
 * the in-driver assertions above as well, not to drop this one.
 */
static void check_image(const char *tag)
{
	char cmd[2048], out[1024];
	int rc;

	snprintf(out, sizeof(out), "/tmp/ttntfs-faults-%d-fsck.txt", (int)getpid());
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' >'%s' 2>&1",
		 tool("NTFS_NTFSCK", "tools/.local-plus/sbin/ntfsck"), img, out);
	rc = system(cmd);
	checks++;
	if (rc != 0) {
		failures++;
		fprintf(stderr, "FAIL %s: ntfsck -n exited %d -- the fault left the volume corrupt\n",
			tag, rc);
		{
			char line[512];
			FILE *f = fopen(out, "r");
			if (f) {
				while (fgets(line, sizeof(line), f))
					fprintf(stderr, "    | %s", line);
				fclose(f);
			}
		}
	}
	unlink(out);
}

/* Cheap content fingerprint (FNV-1a). A mount that failed must leave the image
 * byte-for-byte as it found it: anything else is a half-mount that wrote. */
static uint64_t image_hash(void)
{
	uint8_t buf[1 << 16];
	uint64_t h = 1469598103934665603ULL;
	ssize_t n;
	int fd = open(img, O_RDONLY);

	if (fd < 0)
		return 0;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		for (ssize_t i = 0; i < n; i++)
			h = (h ^ buf[i]) * 1099511628211ULL;
	close(fd);
	return h;
}

static void drop_image(void)
{
	if (img[0])
		unlink(img);
	img[0] = 0;
}

/* ---- fault injection ------------------------------------------------- */

enum { FAULT_NONE = 0, FAULT_READ, FAULT_WRITE, FAULT_FLUSH };

static struct fault {
	int mode;
	uint64_t lo, hi;	/* byte range that arms the fault; hi = 0 means "to the end" */
	int skip;		/* let this many matching transfers through first */
	int times;		/* then fail this many (-1 = every one after that) */
	int err;		/* what the device reports */
	bool partial;		/* do the first half of the transfer for real, then fail */
	/* observed */
	int matched, fired;
	int reads, writes, flushes;
} fi;

static const struct ntfs_bdev_ops *real_ops;
static struct ntfs_bdev_ops fault_ops;

static void fault_clear(void)
{
	memset(&fi, 0, sizeof(fi));
	fi.err = -EIO;
	fi.times = -1;
}

static bool in_range(uint64_t off, size_t count)
{
	uint64_t hi = fi.hi ? fi.hi : UINT64_MAX;
	return off < hi && off + count > fi.lo;
}

static bool should_fail(int mode, uint64_t off, size_t count)
{
	if (fi.mode != mode || !in_range(off, count))
		return false;
	if (fi.matched++ < fi.skip)
		return false;
	if (fi.times >= 0 && fi.fired >= fi.times)
		return false;
	fi.fired++;
	return true;
}

static ssize_t f_pread(struct ntfs_bdev *d, void *b, size_t c, u64 o)
{
	fi.reads++;
	if (should_fail(FAULT_READ, o, c)) {
		/* A real disk that dies partway through a transfer has already
		 * delivered the leading blocks. Reproduce that, so a caller that
		 * looks at the buffer instead of the return value is caught. */
		if (fi.partial) {
			size_t half = (c / 2) & ~(size_t)511;
			if (half)
				real_ops->pread(d, b, half, o);
		}
		return fi.err;
	}
	return real_ops->pread(d, b, c, o);
}

static ssize_t f_pwrite(struct ntfs_bdev *d, const void *b, size_t c, u64 o)
{
	fi.writes++;
	if (should_fail(FAULT_WRITE, o, c)) {
		if (fi.partial) {
			size_t half = (c / 2) & ~(size_t)511;
			if (half)
				real_ops->pwrite(d, b, half, o);
		}
		return fi.err;
	}
	return real_ops->pwrite(d, b, c, o);
}

static int f_flush(struct ntfs_bdev *d)
{
	fi.flushes++;
	if (should_fail(FAULT_FLUSH, 0, 1))
		return fi.err;
	return real_ops->flush(d);
}

static void f_close(struct ntfs_bdev *d) { real_ops->close(d); }

static int f_discard(struct ntfs_bdev *d, u64 o, u64 l)
{
	return real_ops->discard ? real_ops->discard(d, o, l) : -EOPNOTSUPP;
}

/* Open the image and splice our ops in front of the real ones. */
static struct ntfs_bdev *fault_open(bool read_only)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(img, read_only);

	if (!dev)
		return NULL;
	real_ops = dev->ops;
	fault_ops = *real_ops;
	fault_ops.pread = f_pread;
	fault_ops.pwrite = f_pwrite;
	fault_ops.flush = f_flush;
	fault_ops.close = f_close;
	fault_ops.discard = real_ops->discard ? f_discard : NULL;
	dev->ops = &fault_ops;
	return dev;
}

/* ---- boot sector geometry, so a case can name what it breaks --------- */

struct geom {
	uint32_t sector_size, cluster_size, mft_record_size;
	uint64_t mft_off, mftmirr_off;
};

static uint64_t le64_at(const uint8_t *p) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
	return v;
}

static int read_geom(struct geom *g)
{
	uint8_t bs[512];
	int fd = open(img, O_RDONLY);
	int8_t cpr;

	if (fd < 0)
		return -1;
	if (pread(fd, bs, sizeof(bs), 0) != (ssize_t)sizeof(bs)) { close(fd); return -1; }
	close(fd);
	g->sector_size = (uint32_t)bs[0x0b] | ((uint32_t)bs[0x0c] << 8);
	g->cluster_size = g->sector_size * bs[0x0d];
	cpr = (int8_t)bs[0x40];
	g->mft_record_size = cpr > 0 ? (uint32_t)cpr * g->cluster_size : 1u << (uint32_t)(-cpr);
	g->mft_off = le64_at(bs + 0x30) * g->cluster_size;
	g->mftmirr_off = le64_at(bs + 0x38) * g->cluster_size;
	return 0;
}

/* ---- finding a file's data on the raw image -------------------------- */

/*
 * Fault injection by byte range is only as good as our knowledge of where the
 * bytes are. Rather than parse run lists, write a unique marker into the file,
 * sync, and scan the image for it. That gives the exact device offset of the
 * file's first data cluster, which is what "fail this file's data" needs.
 */
static uint64_t find_pattern(const uint8_t *pat, size_t patlen)
{
	uint8_t buf[1 << 16];
	uint64_t off = 0;
	ssize_t n;
	int fd = open(img, O_RDONLY);

	if (fd < 0)
		return UINT64_MAX;
	while ((n = pread(fd, buf, sizeof(buf), (off_t)off)) > (ssize_t)patlen) {
		for (ssize_t i = 0; i + (ssize_t)patlen <= n; i++) {
			if (!memcmp(buf + i, pat, patlen)) {
				close(fd);
				return off + (uint64_t)i;
			}
		}
		off += (uint64_t)n - patlen;
	}
	close(fd);
	return UINT64_MAX;
}

/* ---- mount helpers --------------------------------------------------- */

static const struct ntfs_mount_options RW_OPTS = {
	.flags = 0, .uid = 0, .gid = 0, .fmask = 022, .dmask = 022
};

static int mount_rw(struct ntfs_bdev **dev_out, ntfs_volume_t **vol_out)
{
	struct ntfs_bdev *dev = fault_open(false);
	int err;

	if (!dev)
		return -ENOENT;
	err = ntfs_mount(dev, &RW_OPTS, vol_out);
	if (err) {
		ntfs_bdev_close(dev);
		return err;
	}
	*dev_out = dev;
	return 0;
}

/* Create @name and fill it with @len bytes of @fill, through a clean mount. */
static int seed_file(const char *name, size_t len, uint8_t fill, const uint8_t *marker,
		     size_t markerlen)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	uint8_t *buf;
	int err;

	fault_clear();
	err = mount_rw(&dev, &vol);
	if (err)
		return err;
	if ((err = ntfs_volume_root(vol, &root)) != 0) goto out_umount;
	if ((err = ntfs_create(root, name, 0100644, &f)) != 0) goto out_root;

	buf = malloc(len);
	if (!buf) { err = -ENOMEM; goto out_file; }
	memset(buf, fill, len);
	if (marker && markerlen <= len)
		memcpy(buf, marker, markerlen);
	err = ntfs_write(f, buf, len, 0) == (ssize_t)len ? 0 : -EIO;
	free(buf);
	if (!err)
		err = ntfs_fsync(f, false);
out_file:
	ntfs_inode_put(f);
out_root:
	ntfs_inode_put(root);
out_umount:
	if (ntfs_volume_sync(vol) != 0 && !err)
		err = -EIO;
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	return err;
}

/* ====================================================================== */
/* 0. the harness itself: with injection off, nothing changes             */
/* ====================================================================== */

/*
 * A fault injector that quietly broke the device would make every case below
 * "pass" for the wrong reason. This runs the same mount/write/read/sync path
 * with the wrapper installed and the fault disarmed, and requires it to be
 * indistinguishable from the real device -- including a clean ntfsck.
 */
static void test_injector_is_transparent(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	uint8_t wbuf[65536], rbuf[65536];
	int err;

	if (make_image("transparent", 8) != 0)
		return;
	fault_clear();
	CHECK(mount_rw(&dev, &vol) == 0);
	if (!vol) { drop_image(); return; }

	CHECK(ntfs_volume_root(vol, &root) == 0);
	memset(wbuf, 0x5a, sizeof(wbuf));
	err = ntfs_create(root, "clean.bin", 0100644, &f);
	CHECK(err == 0);
	if (!err) {
		CHECK(ntfs_write(f, wbuf, sizeof(wbuf), 0) == (ssize_t)sizeof(wbuf));
		CHECK(ntfs_fsync(f, false) == 0);
		memset(rbuf, 0, sizeof(rbuf));
		CHECK(ntfs_read(f, rbuf, sizeof(rbuf), 0) == (ssize_t)sizeof(rbuf));
		CHECK(memcmp(wbuf, rbuf, sizeof(rbuf)) == 0);
		ntfs_inode_put(f);
	}
	ntfs_inode_put(root);
	CHECK(ntfs_volume_sync(vol) == 0);
	CHECK(ntfs_unmount(vol) == 0);
	ntfs_bdev_close(dev);

	/* the wrapper really was in the path, it just never fired */
	CHECK_MSG(fi.reads > 0 && fi.writes > 0,
		  "wrapper saw %d reads / %d writes, so it was not in the I/O path at all",
		  fi.reads, fi.writes);
	CHECK(fi.fired == 0);
	check_image("transparent");
	drop_image();
	printf("test_injector_is_transparent\n");
}

/* ====================================================================== */
/* 1. read failures during mount                                          */
/* ====================================================================== */

/*
 * Mount reads the boot sector, then $MFT, then $MFTMirr, then the root index.
 * Break each in turn. The requirement is the same for all four: ntfs_mount()
 * returns a negative errno, hands back no volume, and does not half-mount. A
 * half-mount is the dangerous outcome, because the caller then writes through
 * a volume whose metadata was never loaded.
 */
static void test_read_fail_mount(void)
{
	static const struct { const char *what; int which; int expect_err; } cases[] = {
		{ "boot sector", 0, -EIO },
		{ "$MFT",        1, -EIO },
		{ "root index",  3, -EIO },
		/* $MFTMirr is the one that gets it right-ish: the mirror check
		 * fails, the volume refuses read-write and, with no fallback
		 * requested, the mount is refused with -EROFS. */
		{ "$MFTMirr",    2, -EROFS },
	};
	struct geom g;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct ntfs_bdev *dev;
		ntfs_volume_t *vol = NULL;
		uint64_t before;
		int err;

		if (make_image("mountread", 8) != 0)
			return;
		if (read_geom(&g) != 0) {
			fprintf(stderr, "SKIP read_fail_mount: unreadable boot sector\n");
			drop_image();
			return;
		}

		fault_clear();
		fi.mode = FAULT_READ;
		switch (cases[i].which) {
		case 0:	/* the boot sector is the very first thing read */
			fi.lo = 0; fi.hi = 512;
			break;
		case 1:	/* $MFT record 0, which describes the MFT itself */
			fi.lo = g.mft_off; fi.hi = g.mft_off + g.mft_record_size;
			break;
		case 2:	/* the mirror, checked against the primary at mount */
			fi.lo = g.mftmirr_off; fi.hi = g.mftmirr_off + 4 * g.mft_record_size;
			break;
		case 3:	/* the root directory's MFT record (inode 5) and its index */
			fi.lo = g.mft_off + 5 * g.mft_record_size;
			fi.hi = g.mft_off + 6 * g.mft_record_size;
			break;
		}

		before = image_hash();
		dev = fault_open(false);
		CHECK(dev != NULL);
		if (!dev) { drop_image(); return; }
		err = ntfs_mount(dev, &RW_OPTS, &vol);

		CHECK_MSG(fi.fired > 0, "%s: the fault never fired, so this case tested nothing",
			  cases[i].what);
		CHECK_MSG(err < 0, "%s: mount returned %d with the read failing -- a half-mount",
			  cases[i].what, err);
		CHECK_MSG(err >= 0 || vol == NULL,
			  "%s: mount failed with %d but still handed back a volume", cases[i].what, err);
		printf("  read_fail_mount[%s]: ntfs_mount -> %d (%s)\n", cases[i].what, err,
		       err < 0 ? strerror(-err) : "success");
		/*
		 * The errno has to separate "the device refused to read" from
		 * "this is not an NTFS volume". They were the same -EINVAL until
		 * finding 14 was fixed on 2026-09-14, so a user whose disk was
		 * failing was told their partition is not NTFS, which is the
		 * sentence that makes people reformat. The other direction
		 * matters just as much and is pinned by test_not_ntfs() below:
		 * Disk Arbitration reads -EINVAL as "not mine, try another
		 * driver", so claiming -EIO over a FAT partition would take the
		 * disk away from the driver that can read it.
		 */
		CHECK_MSG(err == cases[i].expect_err,
			  "%s: mount reported %d (%s), this test expected %d (%s)",
			  cases[i].what, err, err < 0 ? strerror(-err) : "success",
			  cases[i].expect_err, strerror(-cases[i].expect_err));
		if (err == 0 && vol)
			ntfs_unmount(vol);
		ntfs_bdev_close(dev);

		/*
		 * A mount that could not read its own metadata must not have
		 * written any. Writing while holding a half-loaded picture of
		 * the volume is how a transient cable fault turns into a
		 * permanently broken filesystem.
		 */
		CHECK_MSG(image_hash() == before,
			  "%s: the failed mount modified the image", cases[i].what);

		/* A failed mount must not have written anything. */
		check_image(cases[i].what);
		drop_image();
	}
	printf("test_read_fail_mount\n");
}

/*
 * The other half of finding 14, and the more dangerous direction to get wrong.
 * -EINVAL from ntfs_mount() is what Disk Arbitration reads as "this volume is
 * not mine", which is how a FAT or APFS partition gets handed to the driver
 * that can actually read it. If the -EIO work above had widened to cover a
 * volume that simply is not NTFS, this driver would start claiming disks it
 * cannot mount and they would stop appearing in Finder at all.
 *
 * Both cases here run over a device that never fails a read, so the only
 * honest answer is "not NTFS".
 */
static void test_not_ntfs_is_einval(void)
{
	static const char *const what[] = { "all zeroes", "a FAT boot sector" };
	char cmd[2048];

	for (int i = 0; i < 2; i++) {
		struct ntfs_bdev *dev;
		ntfs_volume_t *vol = NULL;
		int err;

		snprintf(img, sizeof(img), "/tmp/ttntfs-faults-%d-notntfs.img", (int)getpid());
		snprintf(cmd, sizeof(cmd),
			 "dd if=/dev/zero of='%s' bs=1m count=8 >/dev/null 2>&1", img);
		if (system(cmd) != 0) {
			fprintf(stderr, "SKIP not_ntfs: could not build an image\n");
			img[0] = 0;
			return;
		}
		if (i == 1) {
			/* A plausible non-NTFS boot sector: the jump, an OEM id
			 * that is not "NTFS    ", and the 0xaa55 marker. This is
			 * what the partition types we must not claim look like. */
			uint8_t bs[512];
			int fd = open(img, O_WRONLY);

			memset(bs, 0, sizeof(bs));
			bs[0] = 0xeb; bs[1] = 0x58; bs[2] = 0x90;
			memcpy(bs + 3, "MSDOS5.0", 8);
			bs[510] = 0x55; bs[511] = 0xaa;
			CHECK(fd >= 0 && pwrite(fd, bs, sizeof(bs), 0) == (ssize_t)sizeof(bs));
			if (fd >= 0)
				close(fd);
		}

		fault_clear();		/* no fault: every read succeeds */
		dev = fault_open(false);
		CHECK(dev != NULL);
		if (!dev) { drop_image(); return; }
		err = ntfs_mount(dev, &RW_OPTS, &vol);
		printf("  not_ntfs[%s]: ntfs_mount -> %d (%s)\n", what[i], err,
		       err < 0 ? strerror(-err) : "success");
		CHECK_MSG(err == -EINVAL,
			  "%s: ntfs_mount reported %d (%s), expected -EINVAL -- Disk "
			  "Arbitration needs that to hand the disk to another driver",
			  what[i], err, err < 0 ? strerror(-err) : "success");
		CHECK(fi.fired == 0);
		if (err == 0 && vol)
			ntfs_unmount(vol);
		ntfs_bdev_close(dev);
		drop_image();
	}
	printf("test_not_ntfs_is_einval\n");
}

/* ====================================================================== */
/* 2. read failure after mount, during a file read                        */
/* ====================================================================== */

static const uint8_t MARK[16] = {
	'T','T','N','T','F','S','-','F','A','U','L','T','-','M','R','K'
};

/*
 * The error has to reach the caller of ntfs_read(). If it does not, the
 * application is handed whatever was in the buffer -- zeroes, or the previous
 * tenant of a page -- and told it is file content.
 */
static void test_read_fail_file_data(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	uint8_t rbuf[65536];
	uint64_t data_off;
	ssize_t n;
	int err;

	if (make_image("readdata", 8) != 0)
		return;
	if (seed_file("data.bin", 256 * 1024, 0xa5, MARK, sizeof(MARK)) != 0) {
		fprintf(stderr, "SKIP read_fail_file_data: could not seed\n");
		drop_image();
		return;
	}
	data_off = find_pattern(MARK, sizeof(MARK));
	CHECK_MSG(data_off != UINT64_MAX, "could not locate the file's data on the image");
	if (data_off == UINT64_MAX) { drop_image(); return; }

	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_lookup(root, "data.bin", &f) == 0);

	/* Arm only after the mount is up, so this is unambiguously a data read. */
	fi.mode = FAULT_READ;
	fi.lo = data_off;
	fi.hi = data_off + 128 * 1024;
	memset(rbuf, 0xff, sizeof(rbuf));
	n = ntfs_read(f, rbuf, sizeof(rbuf), 0);

	CHECK_MSG(fi.fired > 0, "the data read never reached the device (cached?), case is vacuous");
	CHECK_MSG(n < 0, "ntfs_read returned %zd for data the device refused to deliver", n);
	if (n < 0)
		printf("  read_fail_file_data: ntfs_read -> %zd (%s)\n", n, strerror((int)-n));

	fault_clear();
	ntfs_inode_put(f);
	ntfs_inode_put(root);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image("read_fail_file_data");
	drop_image();
	printf("test_read_fail_file_data\n");
}

/*
 * A read failure during a directory lookup must not come back as -ENOENT.
 * This is the quietest of all the failure modes and the most destructive: a
 * backup or sync tool that asks "does this file still exist?" and is told no
 * because the index block could not be read will go on to delete the copy it
 * has. The errno has to say "I could not tell", not "it is gone".
 */
static void test_read_fail_lookup(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	char name[64];
	int err, i;

	if (make_image("lookup", 8) != 0)
		return;

	/* Enough entries that the directory index no longer fits in the MFT
	 * record and lookups have to read an INDX block from the device. */
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	for (i = 0; i < 128; i++) {
		snprintf(name, sizeof(name), "entry-%03d-padding-for-a-longer-name.txt", i);
		if (ntfs_create(root, name, 0100644, &f) == 0)
			ntfs_inode_put(f);
	}
	ntfs_inode_put(root);
	CHECK(ntfs_volume_sync(vol) == 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	/* Fresh mount so nothing is cached, then refuse every read. */
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);

	fi.mode = FAULT_READ;
	snprintf(name, sizeof(name), "entry-%03d-padding-for-a-longer-name.txt", 120);
	f = NULL;
	err = ntfs_lookup(root, name, &f);
	CHECK_MSG(fi.fired > 0, "the lookup read nothing from the device, case is vacuous");
	printf("  read_fail_lookup: ntfs_lookup -> %d (%s)\n", err,
	       err ? strerror(-err) : "found");
	CHECK_MSG(err != -ENOENT,
		  "a lookup that could not read the index reported -ENOENT: the caller is "
		  "told the file was deleted when the disk merely failed");
	CHECK_MSG(err < 0, "ntfs_lookup returned %d with every device read refused", err);
	if (!err && f)
		ntfs_inode_put(f);

	fault_clear();
	ntfs_inode_put(root);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image("read_fail_lookup");
	drop_image();
	printf("test_read_fail_lookup\n");
}

/*
 * Two shapes of "the failure does not start on the first block".
 *
 * A large aligned read goes straight from the run list to the device as one
 * transfer (docs/PORTING.md section 3 rule 3), so the only way to break it
 * partway is to have the device deliver its leading blocks and then die -- what
 * a medium error actually looks like. That is case (a), and it is the one that
 * catches a caller trusting the buffer instead of the return value, because the
 * first 64 KB of the buffer is real file data.
 *
 * An unaligned read goes through the page cache instead, one 4 KB transfer per
 * page, so case (b) can let the first blocks through and fail a later one.
 */
static void test_read_fail_midtransfer(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	uint8_t rbuf[131072];
	uint64_t data_off;
	ssize_t n;
	int err;

	if (make_image("readmid", 8) != 0)
		return;
	if (seed_file("data.bin", 512 * 1024, 0xa5, MARK, sizeof(MARK)) != 0) {
		fprintf(stderr, "SKIP read_fail_midtransfer: could not seed\n");
		drop_image();
		return;
	}
	data_off = find_pattern(MARK, sizeof(MARK));
	CHECK_MSG(data_off != UINT64_MAX, "could not locate the file's data on the image");
	if (data_off == UINT64_MAX) { drop_image(); return; }

	/* (a) one 128 KB transfer that delivers half and then fails */
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_lookup(root, "data.bin", &f) == 0);

	fi.mode = FAULT_READ;
	fi.lo = data_off;
	fi.hi = data_off + 512 * 1024;
	fi.partial = true;
	memset(rbuf, 0, sizeof(rbuf));
	n = ntfs_read(f, rbuf, sizeof(rbuf), 0);
	CHECK_MSG(fi.fired > 0, "the torn read never reached the device, case is vacuous");
	CHECK_MSG(n < 0, "ntfs_read returned %zd for a transfer that died halfway -- "
		  "the caller gets half real data and half stale buffer, flagged as success", n);
	printf("  read_fail_midtransfer(torn transfer): ntfs_read -> %zd (%s)\n",
	       n, n < 0 ? strerror((int)-n) : "success");

	ntfs_inode_put(f);
	ntfs_inode_put(root);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	/* (b) a later 4 KB block of a paged transfer, with the earlier ones good */
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_lookup(root, "data.bin", &f) == 0);

	fi.mode = FAULT_READ;
	fi.lo = data_off;
	fi.hi = data_off + 512 * 1024;
	/* The unaligned read is split into a handful of device transfers; let the
	 * first one land and break the next, so the failure is demonstrably not
	 * on the first block of the request. */
	fi.skip = 1;
	fi.times = 1;
	memset(rbuf, 0, sizeof(rbuf));
	/* offset 1024 is unaligned, which keeps this on the page-cache path */
	n = ntfs_read(f, rbuf, 96 * 1024, 1024);
	CHECK_MSG(fi.matched > fi.skip,
		  "only %d transfers hit the data range, cannot fail a later one", fi.matched);
	CHECK_MSG(fi.fired == 1, "the later-block fault fired %d times, expected 1", fi.fired);
	CHECK_MSG(n < 0, "ntfs_read returned %zd although a block in the middle was refused", n);
	printf("  read_fail_midtransfer(later block): ntfs_read -> %zd (%s)\n",
	       n, n < 0 ? strerror((int)-n) : "success");

	fault_clear();
	ntfs_inode_put(f);
	ntfs_inode_put(root);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image("read_fail_midtransfer");
	drop_image();
	printf("test_read_fail_midtransfer\n");
}

/*
 * Fail once, then let the device recover. Nothing may be lost: the same read
 * repeated has to produce the bytes that were written. A driver that caches a
 * failed page as if it were valid fails here and nowhere else.
 */
static void test_transient_read(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	uint8_t rbuf[65536];
	uint64_t data_off;
	ssize_t n;
	size_t i, bad = 0;
	int err;

	if (make_image("transient", 8) != 0)
		return;
	if (seed_file("data.bin", 256 * 1024, 0xa5, MARK, sizeof(MARK)) != 0) {
		drop_image();
		return;
	}
	data_off = find_pattern(MARK, sizeof(MARK));
	if (data_off == UINT64_MAX) { drop_image(); return; }

	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_lookup(root, "data.bin", &f) == 0);

	fi.mode = FAULT_READ;
	fi.lo = data_off;
	fi.hi = data_off + 128 * 1024;
	fi.times = 1;		/* exactly one failure, then the device is well again */
	n = ntfs_read(f, rbuf, sizeof(rbuf), 0);
	CHECK_MSG(fi.fired == 1, "the transient fault fired %d times, expected 1", fi.fired);
	CHECK_MSG(n < 0, "the first read returned %zd, so the transient fault was not observed", n);

	/* and now the recovery: same bytes, no fault left to fire */
	fi.mode = FAULT_NONE;
	memset(rbuf, 0, sizeof(rbuf));
	n = ntfs_read(f, rbuf, sizeof(rbuf), 0);
	CHECK_MSG(n == (ssize_t)sizeof(rbuf), "the retry after a transient error returned %zd", n);
	if (n == (ssize_t)sizeof(rbuf)) {
		CHECK(memcmp(rbuf, MARK, sizeof(MARK)) == 0);
		for (i = sizeof(MARK); i < sizeof(rbuf); i++)
			if (rbuf[i] != 0xa5) { bad++; break; }
		CHECK_MSG(bad == 0, "the retry returned data that is not what was written "
			  "-- a failed read was cached as valid");
	}

	ntfs_inode_put(f);
	ntfs_inode_put(root);
	CHECK(ntfs_volume_sync(vol) == 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image("transient_read");
	drop_image();
	printf("test_transient_read\n");
}

/* ====================================================================== */
/* 3. write failures                                                      */
/* ====================================================================== */

/*
 * Every write to the device fails from the moment the file is created. At least
 * one of ntfs_write(), ntfs_fsync() and ntfs_volume_sync() has to say so; if
 * all three return success the caller has been told its data is on the disk
 * when not one byte of it is, which is the definition of silent corruption.
 */
static void test_write_fail_data(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	struct ntfs_volume_info info;
	uint8_t wbuf[131072];
	ssize_t wn;
	int err, fs_err = 0, sync_err = 0;

	if (make_image("writefail", 8) != 0)
		return;
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	err = ntfs_create(root, "wfail.bin", 0100644, &f);
	CHECK(err == 0);
	if (err) goto out;

	memset(wbuf, 0x3c, sizeof(wbuf));
	fi.mode = FAULT_WRITE;		/* the whole device, every write */
	wn = ntfs_write(f, wbuf, sizeof(wbuf), 0);
	fs_err = ntfs_fsync(f, false);
	sync_err = ntfs_volume_sync(vol);

	CHECK_MSG(fi.fired > 0, "no write reached the device at all, case is vacuous");
	printf("  write_fail_data: ntfs_write=%zd ntfs_fsync=%d ntfs_volume_sync=%d "
	       "(%d device writes refused)\n", wn, fs_err, sync_err, fi.fired);

	/* The error must reach the caller by one of these. It does: the MFT
	 * record write fails first, which fails the write and takes the volume
	 * read-only. */
	CHECK_MSG(wn < 0 || fs_err != 0,
		  "ntfs_write returned %zd and ntfs_fsync returned %d for data no byte of "
		  "which reached the device -- the caller committed to lost data",
		  wn, fs_err);

	/*
	 * ntfs_volume_sync() returning 0 here is not a third opinion: fsync
	 * already reported this error and an error is reported once. What must
	 * still be true is that the volume no longer claims to be writable, so a
	 * caller that ignored the errno cannot keep writing into the void.
	 */
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	printf("  write_fail_data: volume now read_only=%d ro_reason=%d\n",
	       (int)info.read_only, info.ro_reason);
	CHECK_MSG(info.read_only,
		  "the volume is still writable after every metadata write was refused");
	/*
	 * And it must say why. The consequence of not saying is not corruption
	 * but a user who is told their disk is read-only and given no reason,
	 * when the true reason is that the hardware is failing writes and they
	 * should copy their data off now. NTFS_RO_ERRORS exists in ntfscore.h for
	 * exactly this case -- "errors detected while mounted; switched to ro"
	 * -- and until finding 13 was fixed on 2026-09-14 nothing set it: the
	 * reason ladder in super_glue.c ran once at mount, before anything could
	 * fail.
	 */
	CHECK_MSG(info.ro_reason == NTFS_RO_ERRORS,
		  "ro_reason is %d after errors forced the volume read-only, "
		  "expected NTFS_RO_ERRORS (%d)", info.ro_reason, NTFS_RO_ERRORS);

	ntfs_inode_put(f);
out:
	ntfs_inode_put(root);
	fault_clear();		/* let the unmount land, so ntfsck judges the fault, not the teardown */
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	/* The important half: whatever it reported, the volume must be sane. */
	check_image("write_fail_data");
	drop_image();
	printf("test_write_fail_data\n");
}

/*
 * The same fault aimed at metadata writeback rather than file data: create
 * several directory entries, then fail every device write and sync. If
 * ntfs_volume_sync() returns 0 here, an unmount reports success while the MFT
 * and the index on disk disagree with what the caller was told exists.
 */
static void test_write_fail_sync(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	char name[64];
	int err, sync_err, i;

	if (make_image("syncfail", 8) != 0)
		return;
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);

	for (i = 0; i < 8; i++) {
		snprintf(name, sizeof(name), "meta%02d.txt", i);
		err = ntfs_create(root, name, 0100644, &f);
		CHECK(err == 0);
		if (!err) {
			CHECK(ntfs_write(f, "x", 1, 0) == 1);
			ntfs_inode_put(f);
		}
	}

	fi.mode = FAULT_WRITE;
	sync_err = ntfs_volume_sync(vol);
	CHECK_MSG(fi.fired > 0, "ntfs_volume_sync issued no device write, case is vacuous");
	printf("  write_fail_sync: ntfs_volume_sync=%d (%d device writes refused)\n",
	       sync_err, fi.fired);

	/* An unmount that calls sync and believes the 0 it gets back writes the
	 * volume off as clean while the MFT on disk disagrees with what the
	 * caller was told exists. */
	CHECK_MSG(sync_err < 0,
		  "ntfs_volume_sync returned %d with %d metadata writes refused",
		  sync_err, fi.fired);

	ntfs_inode_put(root);
	fault_clear();
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image("write_fail_sync");
	drop_image();
	printf("test_write_fail_sync\n");
}

/*
 * A write that dies partway through a multi-block transfer: the first half of
 * the blocks land, the rest do not. This is what tears a metadata structure in
 * half, so the ntfsck afterwards carries the weight here.
 */
static void test_write_fail_midtransfer(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	uint8_t wbuf[262144];
	ssize_t wn;
	int err, sync_err;

	if (make_image("writemid", 8) != 0)
		return;
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	err = ntfs_create(root, "torn.bin", 0100644, &f);
	CHECK(err == 0);
	if (err) goto out;

	memset(wbuf, 0x77, sizeof(wbuf));
	fi.mode = FAULT_WRITE;
	fi.skip = 3;		/* three transfers land intact */
	fi.times = 1;		/* the fourth tears in half */
	fi.partial = true;
	wn = ntfs_write(f, wbuf, sizeof(wbuf), 0);
	sync_err = ntfs_volume_sync(vol);
	CHECK_MSG(fi.fired == 1, "the torn write fired %d times, expected 1", fi.fired);
	printf("  write_fail_midtransfer: ntfs_write=%zd sync=%d\n", wn, sync_err);
	/* ntfs_write() is buffered and may well return the full count; the sync
	 * is where the caller finds out, and it has to say so. */
	CHECK_MSG(sync_err < 0,
		  "ntfs_write returned %zd and ntfs_volume_sync returned %d for a write "
		  "that landed half on the disk", wn, sync_err);

	ntfs_inode_put(f);
out:
	ntfs_inode_put(root);
	fault_clear();
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image("write_fail_midtransfer");
	drop_image();
	printf("test_write_fail_midtransfer\n");
}

/*
 * The flush is the barrier the whole durability story rests on. If the device
 * refuses it and ntfs_volume_sync() still returns 0, then fsync() lies: the
 * caller believes its data survives a power cut when it is still in a volatile
 * write cache.
 */
static void test_flush_fail(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	int err, sync_err, fs_err = 0;

	if (make_image("flushfail", 8) != 0)
		return;
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	err = ntfs_create(root, "flush.bin", 0100644, &f);
	CHECK(err == 0);
	if (!err)
		CHECK(ntfs_write(f, "hello", 5, 0) == 5);

	fi.mode = FAULT_FLUSH;
	if (!err)
		fs_err = ntfs_fsync(f, false);
	sync_err = ntfs_volume_sync(vol);
	CHECK_MSG(fi.fired > 0, "no flush was issued, case is vacuous");
	printf("  flush_fail: ntfs_fsync=%d ntfs_volume_sync=%d (%d flushes refused)\n",
	       fs_err, sync_err, fi.fired);

	/*
	 * Both of these must report the refused barrier. fsync()'s entire
	 * contract is that the bytes are on stable storage when it returns 0,
	 * and after a refused flush they are still in a write cache that a power
	 * cut empties.
	 *
	 * Where it used to be lost (finding 12, fixed 2026-09-14): core/vfs/
	 * file.c and core/ntfs/super.c both called blkdev_issue_flush() with no
	 * assignment, verbatim from upstream/linux-v7.1/fs/ntfs/. Here the call
	 * is ntfs_bdev_flush(), an F_FULLFSYNC whose errno has nowhere else to
	 * go, so both call sites now keep the return.
	 */
	CHECK_MSG(fs_err == -EIO,
		  "ntfs_fsync returned %d after the device refused the barrier, "
		  "expected -EIO", fs_err);
	CHECK_MSG(sync_err == -EIO,
		  "ntfs_volume_sync returned %d after the device refused the barrier, "
		  "expected -EIO", sync_err);

	if (!err)
		ntfs_inode_put(f);
	ntfs_inode_put(root);
	fault_clear();
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image("flush_fail");
	drop_image();
	printf("test_flush_fail\n");
}

/* ====================================================================== */
/* 4. ENOSPC                                                              */
/* ====================================================================== */

/*
 * Not injected: a genuinely full volume. The allocator is the one place where
 * "report the error" and "do not corrupt anything" pull against each other,
 * because a failure partway through allocating a run has to unwind the cluster
 * bitmap. Requirements: the driver reports ENOSPC rather than some other errno
 * or a short success, the files written before the wall are still readable
 * afterwards, and ntfsck is clean.
 */
static void test_enospc(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	struct ntfs_volume_info info;
	uint8_t buf[65536], rbuf[65536];
	char name[64];
	int err = 0, i, created = 0, hit = 0, last_err = 0;
	ssize_t wn = 0;

	if (make_image("enospc", 6) != 0)
		return;
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	memset(buf, 0x11, sizeof(buf));

	/* Two files we will look for again on the far side of the wall. */
	for (i = 0; i < 2; i++) {
		snprintf(name, sizeof(name), "keep%d.bin", i);
		err = ntfs_create(root, name, 0100644, &f);
		CHECK(err == 0);
		if (!err) {
			CHECK(ntfs_write(f, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
			ntfs_inode_put(f);
		}
	}
	CHECK(ntfs_volume_sync(vol) == 0);

	/* Now fill it. 6 MB of volume, 64 KB a time: a hundred or so writes. */
	for (i = 0; i < 4096 && !hit; i++) {
		snprintf(name, sizeof(name), "fill%04d.bin", i);
		err = ntfs_create(root, name, 0100644, &f);
		if (err) { last_err = err; hit = 1; break; }
		created++;
		wn = ntfs_write(f, buf, sizeof(buf), 0);
		if (wn != (ssize_t)sizeof(buf)) {
			last_err = wn < 0 ? (int)wn : -ENOSPC;
			hit = 1;
		}
		/* Writeback is where the allocator actually runs out. */
		if (!hit) {
			int e = ntfs_fsync(f, false);
			if (e) { last_err = e; hit = 1; }
		}
		ntfs_inode_put(f);
	}

	CHECK_MSG(hit, "wrote %d files into a 6 MB volume without ever hitting the wall",
		  created);
	printf("  enospc: %d files written, then %d (%s)\n",
	       created, last_err, last_err ? strerror(-last_err) : "none");
	CHECK_MSG(last_err == -ENOSPC,
		  "a full volume reported %d (%s), expected -ENOSPC",
		  last_err, last_err ? strerror(-last_err) : "success");

	/* The bitmap must not have been corrupted into claiming free space. */
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	CHECK_MSG(info.free_clusters < info.total_clusters,
		  "a full volume reports %" PRIu64 " of %" PRIu64 " clusters free",
		  info.free_clusters, info.total_clusters);

	/* and the files from before the wall are still there and still right */
	for (i = 0; i < 2; i++) {
		ntfs_inode_t *k;
		snprintf(name, sizeof(name), "keep%d.bin", i);
		err = ntfs_lookup(root, name, &k);
		CHECK_MSG(err == 0, "%s was lost when the volume filled: %d", name, err);
		if (!err) {
			memset(rbuf, 0, sizeof(rbuf));
			CHECK(ntfs_read(k, rbuf, sizeof(rbuf), 0) == (ssize_t)sizeof(rbuf));
			CHECK_MSG(memcmp(rbuf, buf, sizeof(buf)) == 0,
				  "%s came back with different bytes after ENOSPC", name);
			ntfs_inode_put(k);
		}
	}

	ntfs_inode_put(root);
	ntfs_volume_sync(vol);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image("enospc");
	drop_image();
	printf("test_enospc\n");
}

/* A case that bails out early still owns an image; do not litter CI's /tmp. */
static void cleanup(void) { drop_image(); }

int main(void)
{
	atexit(cleanup);
	test_injector_is_transparent();
	test_read_fail_mount();
	test_not_ntfs_is_einval();
	test_read_fail_file_data();
	test_read_fail_lookup();
	test_read_fail_midtransfer();
	test_transient_read();
	test_write_fail_data();
	test_write_fail_sync();
	test_write_fail_midtransfer();
	test_flush_fail();
	test_enospc();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
