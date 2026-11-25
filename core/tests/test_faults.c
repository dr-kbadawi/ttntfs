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

/* A fresh mkntfs volume of @mb megabytes, with @opts added to mkntfs's
 * arguments (NULL for none). Small on purpose: ENOSPC has to be reachable in
 * under a second, and every case wants its own image. */
static int make_image_opts(const char *tag, unsigned mb, const char *opts)
{
	char cmd[2048];

	snprintf(img, sizeof(img), "/tmp/ttntfs-faults-%d-%s.img", (int)getpid(), tag);
	snprintf(cmd, sizeof(cmd),
		 "dd if=/dev/zero of='%s' bs=1m count=%u >/dev/null 2>&1 && "
		 "'%s' -Q -F %s -L FAULT '%s' >/dev/null 2>&1",
		 img, mb, tool("NTFS_MKNTFS", "tools/.local/sbin/mkntfs"),
		 opts ? opts : "", img);
	if (system(cmd) != 0) {
		fprintf(stderr, "SKIP %s: could not build an image (mkntfs missing?)\n", tag);
		img[0] = 0;
		return -1;
	}
	return 0;
}

static int make_image(const char *tag, unsigned mb)
{
	return make_image_opts(tag, mb, NULL);
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
	int content;		/* writes only: arm only for a transfer whose first 16
				 * bytes are all this byte (-1 = any transfer) */
	int skip;		/* let this many matching transfers through first */
	int times;		/* then fail this many (-1 = every one after that) */
	int err;		/* what the device reports */
	bool partial;		/* do the first half of the transfer for real, then fail */
	/* observed */
	int matched, fired;
	uint64_t fired_off;	/* device offset of the first transfer failed */
	int reads, writes, flushes;
} fi = { .content = -1, .err = -EIO, .times = -1 };	/* as fault_clear() leaves it */

static const struct ntfs_bdev_ops *real_ops;
static struct ntfs_bdev_ops fault_ops;

static void fault_clear(void)
{
	memset(&fi, 0, sizeof(fi));
	fi.content = -1;
	fi.err = -EIO;
	fi.times = -1;
}

static bool content_matches(const void *b, size_t count)
{
	const uint8_t *p = b;

	if (fi.content < 0)
		return true;
	if (count < 16)
		return false;
	for (int i = 0; i < 16; i++)
		if (p[i] != (uint8_t)fi.content)
			return false;
	return true;
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
	if (!fi.fired++)
		fi.fired_off = off;
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
	if (content_matches(b, c) && should_fail(FAULT_WRITE, o, c)) {
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
static uint32_t le32_at(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16_at(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

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
 * Fill the free clusters with @fill through one file, then delete it: what a
 * disk that has been used holds where nothing is allocated.
 */
static int stain_free_space(ntfs_volume_t *vol, ntfs_inode_t *root, uint8_t fill,
			    size_t cs)
{
	enum { CHUNK = 65536 };
	uint8_t *buf = malloc(CHUNK);
	ntfs_inode_t *s;
	size_t chunk = CHUNK;
	uint64_t off = 0;
	int err;

	if (!buf)
		return -ENOMEM;
	memset(buf, fill, CHUNK);
	err = ntfs_create(root, "stain.bin", 0100644, &s);
	if (err) {
		free(buf);
		return err;
	}
	for (;;) {
		ssize_t w = ntfs_write(s, buf, chunk, off);

		if (w == (ssize_t)chunk) {
			off += chunk;
			continue;
		}
		if (w != -ENOSPC) {
			err = w < 0 ? (int)w : -EIO;
			break;
		}
		if (chunk <= cs)
			break;
		chunk = cs;
	}
	free(buf);
	/* On the disk before the name goes: a deleted file's dirty pages are
	 * dropped, not written. */
	if (!err)
		err = ntfs_fsync(s, false);
	ntfs_inode_put(s);
	if (!err)
		err = ntfs_unlink(root, "stain.bin");
	if (!err)
		err = ntfs_volume_sync(vol);
	return err;
}

static uint8_t *read_whole_image(size_t *len)
{
	struct stat st;
	uint8_t *p = NULL;
	int fd = open(img, O_RDONLY);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) == 0 && st.st_size > 0)
		p = malloc((size_t)st.st_size);
	if (p && pread(fd, p, (size_t)st.st_size, 0) != (ssize_t)st.st_size) {
		free(p);
		p = NULL;
	}
	close(fd);
	if (p)
		*len = (size_t)st.st_size;
	return p;
}

/*
 * A write into a hole whose data the device refuses after the hole's clusters
 * are mapped: finding 29 in docs/UPSTREAM-BUGS.md. alloc_range() maps every
 * hole a write covers before anything is written, and when a data write then
 * failed, do_write() undid only the sizes. The clusters stayed in the file,
 * inside initialized_size, holding what the disk had there: after a remount,
 * a deleted file's data where the file had a hole, and ntfsck clean.
 * release_holes() gives them back now. test_geometry's
 * test_failed_hole_write() reaches the same path through -ENOSPC. This is the
 * other way in, and on an idle volume the only one for a named stream:
 * ntfs_setxattr() writes a whole value into an emptied stream, one hole that
 * the allocator fills entirely or refuses up front. For the stream,
 * do_write()'s truncate back to the empty size frees the clusters anyway; the
 * case is there to run release_holes() on an attribute inode, which
 * ntfs_non_resident_attr_punch_hole() refuses, and to see the volume clean
 * after it.
 *
 * A second file takes a write that starts inside its hole and runs past its
 * end, so that the release comes before do_write()'s truncate back to the old
 * size, and gives back part of a hole next to a part that stays one.
 *
 * The holes are read on the same mount too, before the remount: the page
 * cache must not show what the failed writes left there either. And where a
 * page holds more than one cluster (@mkopts makes them 512 bytes), the first
 * file's page 0 also holds W1, an earlier write's dirty bytes in a cluster
 * that stays. W1 is 'W' as well, so the device refuses it while the fault is
 * armed, as a device that refuses writes for a while would: the release must
 * not write that page back and drop it, or W1 is gone and a later fsync says
 * nothing. W1 has to reach the disk, or fsync has to say it did not.
 *
 * The free clusters hold 'D' first. The fault fails only transfers that begin
 * with the 'W' the case writes, so the zeroing and every metadata write land.
 */
#define HOLE_XATTR "fail.into.hole"

/*
 * Bytes of [from, to) of @f that are not zero, -1 if a read fails. No read
 * spans a whole page: do_read() writes back and drops a whole page before it
 * reads the device directly, so only reads within a page show what the page
 * cache holds.
 */
static long long cached_nonzero(ntfs_inode_t *f, uint8_t *rb, uint64_t from, uint64_t to)
{
	const uint64_t pg = 1u << NTFS_PAGE_SHIFT;
	long long n = 0;

	for (uint64_t at = from, stop; at < to; at = stop) {
		stop = (at / pg + 1) * pg;
		if (stop > to)
			stop = to;
		if (stop - at == pg)
			stop = at + 1;
		if (ntfs_read(f, rb, stop - at, at) != (ssize_t)(stop - at))
			return -1;
		for (uint64_t b = 0; b < stop - at; b++)
			n += rb[b] != 0;
	}
	return n;
}

static void test_write_fail_into_hole(const char *tag, const char *mkopts)
{
	enum { WLEN = 32768 };
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	struct ntfs_volume_info info;
	struct ntfs_attr a = { 0 };
	uint8_t *buf = NULL, *pre = NULL, *rb = NULL;
	uint64_t cs, hole_end, free_before = 0, alloc_before = 0, ext_before = 0;
	uint64_t off = 0, r;
	size_t pre_len = 0, rlen, len = 0;
	long long nz;
	ssize_t wn;
	int err, xerr, w1_sync = 0;
	bool small;

	if (make_image_opts(tag, 8, mkopts) != 0)
		return;
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	cs = info.cluster_size;
	small = cs < (1u << NTFS_PAGE_SHIFT);
	/* The hole is [1, hole_end), at least 64 KiB, so that the write below
	 * has whole pages in its middle, which go to the device directly. */
	hole_end = 1 + (65536 / cs > 8 ? 65536 / cs : 8);
	rlen = (size_t)(hole_end + 1) * cs;
	buf = malloc(WLEN + 3 * cs);
	rb = malloc(rlen);
	if (!buf || !rb) {
		CHECK_MSG(0, "%s: out of memory", tag);
		ntfs_inode_put(root);
		goto out_umount;
	}
	memset(buf, 'F', cs);
	err = ntfs_create(root, "sparse.bin", 0100644, &f);
	CHECK(err == 0);
	if (!err) {
		CHECK(ntfs_write(f, buf, cs, 0) == (ssize_t)cs);
		CHECK(ntfs_write(f, buf, cs, hole_end * cs) == (ssize_t)cs);
		CHECK(ntfs_fsync(f, false) == 0);
		ntfs_inode_put(f);
		/* Clusters at 0 and 4: the hole is [1, 4), the end at 5. */
		err = ntfs_create(root, "extend.bin", 0100644, &f);
		CHECK(err == 0);
	}
	if (!err) {
		CHECK(ntfs_write(f, buf, cs, 0) == (ssize_t)cs);
		CHECK(ntfs_write(f, buf, cs, 4 * cs) == (ssize_t)cs);
		CHECK(ntfs_fsync(f, false) == 0);
		ntfs_inode_put(f);
		err = ntfs_create(root, "stream.bin", 0100644, &f);
		CHECK(err == 0);
	}
	if (!err) {
		ntfs_inode_put(f);
		err = stain_free_space(vol, root, 'D', cs);
		CHECK_MSG(err == 0, "%s: staining the free clusters failed with %d", tag, err);
	}
	ntfs_inode_put(root);
out_umount:
	CHECK(ntfs_volume_sync(vol) == 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err || !buf || !rb)
		goto out;
	pre = read_whole_image(&pre_len);
	CHECK_MSG(pre != NULL, "%s: cannot read the image back", tag);

	/* The writes, with the device refusing their data. */
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err)
		goto out;
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	free_before = info.free_clusters;
	memset(buf, 'W', WLEN + 3 * cs);
	fi.mode = FAULT_WRITE;
	fi.content = 'W';
	err = ntfs_lookup(root, "sparse.bin", &f);
	CHECK(err == 0);
	if (!err) {
		CHECK(ntfs_getattr(f, &a) == 0);
		alloc_before = a.alloc_size;
		/* W1 lands in the page cache only: no transfer to refuse yet. */
		if (small)
			CHECK_MSG(ntfs_write(f, buf, cs, 0) == (ssize_t)cs,
				  "%s: W1 into cluster 0 did not write", tag);
		wn = ntfs_write(f, buf, WLEN, cs + 100);
		CHECK_MSG(wn < 0, "%s: a write into a hole whose data the device refused "
			  "returned %zd", tag, wn);
		CHECK_MSG(fi.fired > 0, "%s: no transfer of the data reached the device: "
			  "the case is vacuous", tag);
		off = fi.fired_off;
		ntfs_inode_put(f);
	}
	err = ntfs_lookup(root, "extend.bin", &f);
	CHECK(err == 0);
	if (!err) {
		CHECK(ntfs_getattr(f, &a) == 0);
		ext_before = a.alloc_size;
		fi.fired = 0;
		wn = ntfs_write(f, buf, WLEN + 3 * cs, 2 * cs + 100);
		CHECK_MSG(wn < 0, "%s: a write from a hole past the end whose data the "
			  "device refused returned %zd", tag, wn);
		CHECK_MSG(fi.fired > 0, "%s: no transfer of the extending write reached "
			  "the device: the case is vacuous", tag);
		ntfs_inode_put(f);
	}
	err = ntfs_lookup(root, "stream.bin", &f);
	CHECK(err == 0);
	if (!err) {
		fi.fired = 0;
		xerr = ntfs_setxattr(f, HOLE_XATTR, buf, WLEN, 0);
		CHECK_MSG(xerr < 0, "%s: a stream value the device refused returned %d",
			  tag, xerr);
		CHECK_MSG(fi.fired > 0, "%s: no transfer of the stream's value reached the "
			  "device: the case is vacuous", tag);
		ntfs_inode_put(f);
	}
	fault_clear();

	/* The same mount: what the page cache shows over the holes. */
	err = ntfs_lookup(root, "sparse.bin", &f);
	CHECK(err == 0);
	if (!err) {
		nz = cached_nonzero(f, rb, cs, hole_end * cs);
		CHECK_MSG(nz == 0, "%s: before a remount, %lld bytes of sparse.bin's hole "
			  "read back non-zero after the failed write", tag, nz);
		if (small)
			w1_sync = ntfs_fsync(f, false);
		ntfs_inode_put(f);
	}
	err = ntfs_lookup(root, "extend.bin", &f);
	CHECK(err == 0);
	if (!err) {
		nz = cached_nonzero(f, rb, cs, 4 * cs);
		CHECK_MSG(nz == 0, "%s: before a remount, %lld bytes of extend.bin's hole "
			  "read back non-zero after the failed write", tag, nz);
		ntfs_inode_put(f);
	}
	ntfs_inode_put(root);
	CHECK(ntfs_volume_sync(vol) == 0);
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	CHECK_MSG(!info.read_only, "%s: the refused data took the volume read-only, so "
		  "nothing the failed writes left behind reached the disk", tag);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	/* The refused data was aimed at a cluster that held the deleted
	 * file's 'D': without release_holes() it is what the hole shows. */
	{
		size_t k = 0;

		while (pre && off + k < pre_len && k < 16 && pre[off + k] == 'D')
			k++;
		CHECK_MSG(k == 16, "%s: the refused write was aimed at byte %" PRIu64 ", "
			  "which held no 'D': this case tested nothing", tag, off);
	}

	/* What a remount reads back. */
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err)
		goto out;
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	CHECK_MSG(info.free_clusters == free_before,
		  "%s: %" PRIu64 " clusters free after the failed writes, %" PRIu64
		  " before them", tag, info.free_clusters, free_before);
	err = ntfs_lookup(root, "sparse.bin", &f);
	CHECK(err == 0);
	if (!err) {
		size_t stale = 0, deleted = 0, written = 0, w1 = 0;

		CHECK(ntfs_getattr(f, &a) == 0);
		CHECK_MSG(a.alloc_size == alloc_before,
			  "%s: sparse.bin has %" PRIu64 " bytes allocated after the failed "
			  "write, %" PRIu64 " before it", tag, a.alloc_size, alloc_before);
		CHECK(ntfs_read(f, rb, rlen, 0) == (ssize_t)rlen);
		for (size_t b = 0; b < rlen; b++) {
			uint64_t c = b / cs;

			if (c == 0 && small) {
				w1 += rb[b] == 'W';
				continue;
			}
			stale += rb[b] != ((c == 0 || c == hole_end) ? 'F' : 0);
			deleted += rb[b] == 'D';
			written += rb[b] == 'W';
		}
		CHECK_MSG(stale == 0,
			  "%s: after the failed write, %zu bytes of sparse.bin read back wrong "
			  "after a remount (%zu are the deleted file's 'D', %zu the failed "
			  "write's 'W')", tag, stale, deleted, written);
		if (small)
			CHECK_MSG(w1 == cs || w1_sync != 0,
				  "%s: W1, written before the failed write into the same page, "
				  "is not on the disk (%zu of %" PRIu64 " bytes), and fsync "
				  "returned %d", tag, w1, cs, w1_sync);
		CHECK_MSG(ntfs_seek_data_hole(f, 0, true, &r) == 0 && r == cs,
			  "%s: the hole is not a hole again", tag);
		ntfs_inode_put(f);
	}
	err = ntfs_lookup(root, "extend.bin", &f);
	CHECK(err == 0);
	if (!err) {
		size_t stale = 0, deleted = 0, other = 0;

		CHECK(ntfs_getattr(f, &a) == 0);
		CHECK_MSG(a.size == 5 * cs, "%s: extend.bin is %" PRIu64 " bytes after the "
			  "failed write, %" PRIu64 " before it", tag, a.size, 5 * cs);
		CHECK_MSG(a.alloc_size == ext_before,
			  "%s: extend.bin has %" PRIu64 " bytes allocated after the failed "
			  "write, %" PRIu64 " before it", tag, a.alloc_size, ext_before);
		CHECK(ntfs_read(f, rb, 5 * cs, 0) == (ssize_t)(5 * cs));
		/* Cluster 4 was the file's own, and the write covered it: where a
		 * page holds more than one cluster, the edge page took the failed
		 * write's bytes there. Its own bytes or those, nothing else. */
		for (size_t b = 0; b < 5 * cs; b++) {
			uint64_t c = b / cs;

			if (c == 4)
				other += rb[b] != 'F' && rb[b] != 'W';
			else
				stale += rb[b] != (c == 0 ? 'F' : 0);
			deleted += rb[b] == 'D';
		}
		CHECK_MSG(stale == 0 && other == 0,
			  "%s: after the failed extending write, %zu bytes of extend.bin's "
			  "hole and %zu of its last cluster read back wrong after a remount "
			  "(%zu are the deleted file's 'D')", tag, stale, other, deleted);
		CHECK_MSG(ntfs_seek_data_hole(f, cs, false, &r) == 0 && r == 4 * cs,
			  "%s: data inside extend.bin's hole", tag);
		ntfs_inode_put(f);
	}
	err = ntfs_lookup(root, "stream.bin", &f);
	CHECK(err == 0);
	if (!err) {
		xerr = ntfs_getxattr(f, HOLE_XATTR, NULL, 0, &len);
		CHECK_MSG(xerr == -ENOATTR || (xerr == 0 && len == 0),
			  "%s: after the failed setxattr the stream reads %d, %zu bytes",
			  tag, xerr, len);
		memset(buf, 'F', cs);
		CHECK_MSG(ntfs_setxattr(f, HOLE_XATTR, buf, cs, 0) == 0,
			  "%s: a value did not go in after the failed one", tag);
		CHECK_MSG(ntfs_getxattr(f, HOLE_XATTR, rb, cs, &len) == 0 && len == cs &&
			  memcmp(rb, buf, cs) == 0, "%s: the stream did not read back", tag);
		ntfs_inode_put(f);
	}
	ntfs_inode_put(root);
	CHECK(ntfs_volume_sync(vol) == 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image(tag);
out:
	free(buf);
	free(pre);
	free(rb);
	drop_image();
	printf("test_write_fail_into_hole (%s)\n", tag);
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

/*
 * ENOSPC from metadata (finding 26): the wall is hit by new names, not by
 * data. Every case first fills all clusters with 4 KiB files and asserts
 * that none is left. The first version of this test reached its wall on a
 * 2 MB volume with 70 of 511 clusters still free. That ENOSPC came from an
 * allocator bug (upstream 18d4d2a3ce75, not in this port), not from a full
 * disk. Two walls, both on 4 MB volumes:
 *
 *   index: 300 tiny resident files are created before the fill and deleted
 *     after it, which leaves MFT records free but no clusters. New names in
 *     the root then fill an index block, and splitting it needs a cluster.
 *     Upstream's ntfs_enlarge_attribute() dropped that -ENOSPC and returned
 *     -1, so create() failed with EPERM (U10).
 *   mft: a directory gets 400 names, which are then deleted, so it keeps
 *     index blocks with room in them. New names there use up $MFT's records,
 *     and there is no cluster for it to grow into.
 *     ntfs_mft_data_extend_allocation_nolock() and ntfs_mft_record_alloc()
 *     reported that through ntfs_error(), which sets the volume read-only.
 *     That create got ENOSPC, and every write after it EROFS (U11).
 *
 * Requirements, test_enospc's plus one:
 *   - the create fails with ENOSPC and nothing else;
 *   - the volume stays writable: free the last eight names, and the same
 *     create succeeds;
 *   - after a remount, the file from before the fill and the re-created name
 *     are both there (a cached read would prove nothing);
 *   - ntfsck is clean.
 * A full disk is an ordinary condition and must not cost the user the volume.
 */
static void enospc_names(const char *tag, bool mft)
{
	static const char keep[] = "written before the wall";
	static uint8_t block[4096];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root = NULL, *dir = NULL, *f;
	struct ntfs_volume_info info;
	char name[64], again[64] = "", rbuf[sizeof(keep)];
	int err, i, bad = 0, created = 0, last_err = 0;

	if (make_image(tag, 4) != 0)
		return;
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	err = ntfs_volume_root(vol, &root);
	CHECK(err == 0);
	if (err)
		goto unmount;

	err = ntfs_create(root, "keep.txt", 0100644, &f);
	CHECK(err == 0);
	if (!err) {
		CHECK(ntfs_write(f, keep, sizeof(keep), 0) == (ssize_t)sizeof(keep));
		ntfs_inode_put(f);
	}

	if (mft) {
		err = ntfs_mkdir(root, "d", 040755, &dir);
		CHECK(err == 0);
		if (err)
			goto put;
		for (i = 0; i < 400; i++) {
			snprintf(name, sizeof(name), "p%05d", i);
			if (ntfs_create(dir, name, 0100644, &f)) {
				bad++;
				continue;
			}
			ntfs_inode_put(f);
		}
		for (i = 0; i < 400; i++) {
			snprintf(name, sizeof(name), "p%05d", i);
			bad += ntfs_unlink(dir, name) != 0;
		}
		CHECK_MSG(bad == 0, "%s: %d of 800 setup operations in d failed", tag, bad);
	} else {
		dir = root;
		ntfs_inode_ref(dir);
		for (i = 0; i < 300; i++) {
			snprintf(name, sizeof(name), "t%03d", i);
			if (ntfs_create(root, name, 0100644, &f)) {
				bad++;
				continue;
			}
			bad += ntfs_write(f, "x", 1, 0) != 1;
			ntfs_inode_put(f);
		}
		CHECK_MSG(bad == 0, "%s: %d of 300 tiny files failed", tag, bad);
	}

	/* Fill every cluster. */
	memset(block, 0x11, sizeof(block));
	for (i = 0; i < 100000; i++) {
		ssize_t wn;

		snprintf(name, sizeof(name), "fill%05d", i);
		err = ntfs_create(root, name, 0100644, &f);
		if (err)
			break;
		wn = ntfs_write(f, block, sizeof(block), 0);
		if (wn == (ssize_t)sizeof(block))
			err = ntfs_fsync(f, false);
		else
			err = wn < 0 ? (int)wn : -ENOSPC;
		ntfs_inode_put(f);
		if (err)
			break;
	}
	CHECK_MSG(err == -ENOSPC, "%s: filling the clusters ended with %d (%s)",
		  tag, err, strerror(-err));
	if (!mft) {
		for (i = 0, bad = 0; i < 300; i++) {
			snprintf(name, sizeof(name), "t%03d", i);
			bad += ntfs_unlink(root, name) != 0;
		}
		CHECK_MSG(bad == 0, "%s: %d of 300 tiny files could not be unlinked",
			  tag, bad);
	}
	err = ntfs_volume_get_info(vol, &info);
	CHECK(err == 0);
	if (!err)
		CHECK_MSG(info.free_clusters == 0,
			  "%s: %" PRIu64 " clusters still free after the fill; "
			  "the wall below would not be a full volume",
			  tag, info.free_clusters);

	for (i = 0; i < 5000; i++) {
		snprintf(name, sizeof(name), "z%05dxx", i);
		err = ntfs_create(dir, name, 0100644, &f);
		if (err) {
			last_err = err;
			break;
		}
		ntfs_inode_put(f);
		created++;
	}
	printf("  %s: %d names created on a full volume, then %d (%s)\n", tag,
	       created, last_err, last_err ? strerror(-last_err) : "none");
	CHECK_MSG(last_err != 0, "%s: the wall was never hit", tag);
	CHECK_MSG(last_err == -ENOSPC,
		  "%s: a full volume reported %d (%s), expected -ENOSPC",
		  tag, last_err, strerror(-last_err));
	err = ntfs_volume_get_info(vol, &info);
	CHECK(err == 0);
	if (!err)
		CHECK_MSG(!info.read_only,
			  "%s: running out of space set the volume read-only (ro_reason %d)",
			  tag, info.ro_reason);

	/* Still writable: free the last eight names and create one of them again. */
	for (i = created - 1; i >= 0 && i >= created - 8; i--) {
		snprintf(name, sizeof(name), "z%05dxx", i);
		err = ntfs_unlink(dir, name);
		CHECK_MSG(err == 0, "%s: unlink %s after ENOSPC: %d (%s)",
			  tag, name, err, strerror(-err));
	}
	snprintf(again, sizeof(again), "z%05dxx", created > 8 ? created - 8 : 0);
	err = ntfs_create(dir, again, 0100644, &f);
	CHECK_MSG(err == 0, "%s: create %s after freeing eight names: %d (%s)",
		  tag, again, err, strerror(-err));
	if (!err)
		ntfs_inode_put(f);

put:
	if (dir)
		ntfs_inode_put(dir);
	ntfs_inode_put(root);
unmount:
	CHECK(ntfs_volume_sync(vol) == 0);
	CHECK(ntfs_unmount(vol) == 0);
	ntfs_bdev_close(dev);

	/* Read back after a remount, not from the cache. */
	err = mount_rw(&dev, &vol);
	CHECK_MSG(err == 0, "%s: remount failed: %d", tag, err);
	if (!err) {
		ntfs_inode_t *r = NULL, *d = NULL;

		if (ntfs_volume_root(vol, &r) == 0) {
			err = ntfs_lookup(r, "keep.txt", &f);
			CHECK_MSG(err == 0, "%s: keep.txt lost when the volume filled: %d",
				  tag, err);
			if (!err) {
				memset(rbuf, 0, sizeof(rbuf));
				CHECK(ntfs_read(f, rbuf, sizeof(rbuf), 0) == (ssize_t)sizeof(rbuf));
				CHECK_MSG(memcmp(rbuf, keep, sizeof(keep)) == 0,
					  "%s: keep.txt came back with different bytes", tag);
				ntfs_inode_put(f);
			}
			if (mft)
				CHECK(ntfs_lookup(r, "d", &d) == 0);
			else
				ntfs_inode_ref(d = r);
			if (d && again[0]) {
				err = ntfs_lookup(d, again, &f);
				CHECK_MSG(err == 0, "%s: %s, created after ENOSPC, is gone "
					  "after a remount: %d", tag, again, err);
				if (!err)
					ntfs_inode_put(f);
			}
			if (d)
				ntfs_inode_put(d);
			ntfs_inode_put(r);
		} else {
			CHECK(0);
		}
		CHECK(ntfs_unmount(vol) == 0);
		ntfs_bdev_close(dev);
	}
	check_image(tag);
	drop_image();
}

static void test_enospc_names(void)
{
	enospc_names("enospc-index", false);
	enospc_names("enospc-mft", true);
	printf("test_enospc_names\n");
}

/*
 * Create files named @fmt % 0, 1, ... in @dir until a create fails, writing
 * @payload (with its NUL) into each unless it is NULL. Returns how many.
 */
static int fill_names(ntfs_inode_t *dir, const char *fmt, const char *payload,
		      int *last_err)
{
	ntfs_inode_t *f;
	char name[64];
	int i, err = 0;

	for (i = 0; i < 20000; i++) {
		snprintf(name, sizeof(name), fmt, i);
		err = ntfs_create(dir, name, 0100644, &f);
		if (err)
			break;
		if (payload && ntfs_write(f, payload, strlen(payload) + 1, 0) !=
				(ssize_t)strlen(payload) + 1)
			err = -EIO;
		ntfs_inode_put(f);
		if (err)
			break;
	}
	*last_err = err;
	return i;
}

/* After a remount: @fmt % [0, @n) are all still in @dir, each holding
 * @payload unless it is NULL. */
static void check_names(const char *tag, ntfs_inode_t *dir, const char *fmt, int n,
			const char *payload)
{
	ntfs_inode_t *f;
	char name[64], first[64] = "", rbuf[64];
	int i, lost = 0, changed = 0;

	for (i = 0; i < n; i++) {
		snprintf(name, sizeof(name), fmt, i);
		if (ntfs_lookup(dir, name, &f) != 0) {
			if (!lost++ && !changed)
				snprintf(first, sizeof(first), "%s", name);
			continue;
		}
		if (payload) {
			ssize_t len = (ssize_t)strlen(payload) + 1;
			bool same = false;

			memset(rbuf, 0, sizeof(rbuf));
			if (len <= (ssize_t)sizeof(rbuf))
				same = ntfs_read(f, rbuf, (size_t)len, 0) == len &&
				       !memcmp(rbuf, payload, (size_t)len);
			if (!same && !changed++ && !lost)
				snprintf(first, sizeof(first), "%s", name);
		}
		ntfs_inode_put(f);
	}
	CHECK_MSG(lost == 0 && changed == 0,
		  "%s: after a remount %d of %d names are gone and %d hold other bytes, the first %s",
		  tag, lost, n, changed, first);
}

/* Fill every free cluster with 4 KiB files in @dir. Returns what stopped it. */
static int fill_clusters(ntfs_inode_t *dir)
{
	static uint8_t block[4096];
	ntfs_inode_t *f;
	char name[64];
	int i, err = 0;

	memset(block, 0x11, sizeof(block));
	for (i = 0; i < 100000; i++) {
		ssize_t wn;

		snprintf(name, sizeof(name), "fill%05d", i);
		err = ntfs_create(dir, name, 0100644, &f);
		if (err)
			break;
		wn = ntfs_write(f, block, sizeof(block), 0);
		if (wn == (ssize_t)sizeof(block))
			err = ntfs_fsync(f, false);
		else
			err = wn < 0 ? (int)wn : -ENOSPC;
		ntfs_inode_put(f);
		if (err)
			break;
	}
	return err;
}

/*
 * $EA_INFORMATION summarises $EA: its query length is the size of the EA list.
 * Count the records in use where the two disagree, and report the first. Not
 * redundant with ntfsck, which does not compare them (measured, finding 27).
 * Scans the unmounted image for FILE records rather than following $MFT's run
 * list: on these volumes the only FILE records are $MFT's and $MFTMirr's.
 */
static int ea_disagreements(uint32_t *first)
{
	struct geom g;
	uint8_t *rec;
	uint64_t off;
	int fd, n = 0;

	if (read_geom(&g) != 0 || (fd = open(img, O_RDONLY)) < 0)
		return -1;
	rec = malloc(g.mft_record_size);
	if (!rec) {
		close(fd);
		return -1;
	}
	for (off = 0; pread(fd, rec, g.mft_record_size, (off_t)off) ==
			(ssize_t)g.mft_record_size; off += g.mft_record_size) {
		uint32_t a, len, used = le32_at(rec + 0x18), query = 0;
		uint16_t usa = le16_at(rec + 4), usa_n = le16_at(rec + 6);
		uint64_t ea = 0;
		bool has_info = false, has_ea = false;

		if (memcmp(rec, "FILE", 4) || !(le16_at(rec + 0x16) & 1) ||
		    usa + 2u * usa_n > g.mft_record_size || used > g.mft_record_size)
			continue;
		/* Undo the update sequence: the last two bytes of each 512-byte
		 * block, NTFS's stride whatever the sector size. */
		for (uint16_t i = 1; i < usa_n && i * 512u <= g.mft_record_size; i++)
			memcpy(rec + i * 512u - 2, rec + usa + 2 * i, 2);
		for (a = le16_at(rec + 0x14); a + 24 <= used; a += len) {
			uint32_t type = le32_at(rec + a);

			len = le32_at(rec + a + 4);
			if (type == 0xffffffff || len < 24 || len > used - a)
				break;
			if (type == 0xd0 && !rec[a + 8] &&	/* $EA_INFORMATION, resident */
			    le16_at(rec + a + 0x14) + 8u <= len) {
				has_info = true;
				query = le32_at(rec + a + le16_at(rec + a + 0x14) + 4);
			} else if (type == 0xe0) {		/* $EA */
				has_ea = true;
				if (!rec[a + 8])
					ea = le32_at(rec + a + 0x10);
				else	/* data_size; a short record cannot match */
					ea = len >= 0x40 ? le64_at(rec + a + 0x30) : UINT64_MAX;
			}
		}
		if ((has_info || has_ea) && (has_info != has_ea || query != ea) && !n++)
			*first = le32_at(rec + 0x2c);
	}
	free(rec);
	close(fd);
	return n;
}

/* The unmounted image: ntfsck clean, and every EA summary matching its EAs. */
static void check_reuse_image(const char *tag)
{
	uint32_t first = 0;
	int bad;

	check_image(tag);
	bad = ea_disagreements(&first);
	CHECK_MSG(bad == 0, "%s: %d records whose $EA_INFORMATION disagrees with $EA, the first %u",
		  tag, bad, first);
}

/*
 * A create that fails late must leave nothing behind (finding 27, U12).
 * __ntfs_create() writes the new file's $LXUID/$LXGID/$LXMOD through
 * attribute inodes, which stay cached after use, each holding a reference on
 * the file's inode. When a later step failed -- here the directory index has
 * no room left -- the error path freed the MFT record but left them cached,
 * so its iput() never evicted the dead inode. Once the record was given to
 * another file:
 *
 *   - The next sync wrote the dead inode's copy of the record, marked not in
 *     use, over the new file's. The new name stayed in its folder and pointed
 *     at nothing: after a remount the file was gone, and ntfsck reported the
 *     dangling entry and an $MFT bitmap bit set for a record not in use
 *     (first noted as a one-record bitmap leak, finding 26).
 *   - The new file's own EA writes found those attribute inodes, which match
 *     by record number, type and name only, and went into the dead inode. Its
 *     $EA_INFORMATION stayed zero beside a one-entry $EA. ntfsck does not see
 *     that; ea_disagreements() does.
 *
 * The bug needs a create that fails after it has taken an MFT record, and a
 * later file given that record. The first version of this test got both from
 * a 2 MB volume's allocator wall, which upstream 18d4d2a3ce75 (U13) moves: at
 * the real wall the failing create dies extending $MFT, before it has a
 * record. reuse_fill() then passed without reaching the bug, and reuse_data()
 * failed on an unrelated ENOSPC. So the volume is made full first, as in
 * finding 26's test, and the record is pinned rather than hoped for:
 *
 *   1. With MFT records free but no cluster, names go into the root until its
 *      index needs a new block: that create fails after taking a record.
 *   2. Names go into d, which has index room, until $MFT has no record left.
 *   3. One of them is unlinked: its record K is then the only one free. A
 *      create must be given K (finding 28: the record search used to stop at
 *      a bit past the end of $MFT and report ENOSPC). It is unlinked again.
 *      The search reaches such a bit only while $MFT's record count is not a
 *      multiple of 8 and K lies below the bitmap byte holding its last
 *      records, so the test checks both first.
 *   4. The root's failing name again: it must take K, and fail as in 1.
 *   5. A new file in d must be given K. The test checks that it was, so it
 *      cannot pass without reaching the bug.
 *
 * Every file in d is written 30 bytes. After an unmount, ntfsck must be clean
 * and every record's EA summary must match its EAs; after a remount, every
 * name must be there with its bytes. The record pinning is from the review of
 * PR #8 and its allocator follow-up, which found the first version vacuous.
 * The sequence is the same with upstream's allocator fixes (U13, U14).
 */
static void test_failed_create_record_reuse(void)
{
	static const char payload[] = "precious bytes the user saved";
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *d = NULL, *f;
	struct ntfs_volume_info info;
	struct ntfs_attr at;
	char name[64], wall[64] = "", rbuf[sizeof(payload)];
	int err, i, bad = 0, in_root = 0, in_d = 0, last_err = 0;
	uint64_t k = 0, got = 0;

	if (make_image("reuse", 4) != 0)
		return;
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);

	/* d: 400 names made and deleted, so it keeps index room without a cluster. */
	err = ntfs_mkdir(root, "d", 040755, &d);
	CHECK(err == 0);
	if (err)
		goto unmount;
	for (i = 0; i < 400; i++) {
		snprintf(name, sizeof(name), "p%05d", i);
		if (ntfs_create(d, name, 0100644, &f)) {
			bad++;
			continue;
		}
		ntfs_inode_put(f);
	}
	for (i = 0; i < 400; i++) {
		snprintf(name, sizeof(name), "p%05d", i);
		bad += ntfs_unlink(d, name) != 0;
	}
	/* 300 tiny files hold MFT records through the fill, then give them back. */
	for (i = 0; i < 300; i++) {
		snprintf(name, sizeof(name), "t%03d", i);
		if (ntfs_create(root, name, 0100644, &f)) {
			bad++;
			continue;
		}
		bad += ntfs_write(f, "x", 1, 0) != 1;
		ntfs_inode_put(f);
	}
	err = fill_clusters(root);
	CHECK_MSG(err == -ENOSPC, "reuse: filling the clusters ended with %d (%s)",
		  err, strerror(-err));
	for (i = 0; i < 300; i++) {
		snprintf(name, sizeof(name), "t%03d", i);
		bad += ntfs_unlink(root, name) != 0;
	}
	CHECK_MSG(bad == 0, "reuse: %d setup operations failed", bad);
	memset(&info, 0, sizeof(info));
	err = ntfs_volume_get_info(vol, &info);
	CHECK_MSG(err == 0 && info.free_clusters == 0,
		  "reuse: after the fill, get_info %d, %" PRIu64 " clusters free",
		  err, info.free_clusters);

	/* 1. The root, to its index wall. */
	in_root = fill_names(root, "z%05dxx", NULL, &last_err);
	snprintf(wall, sizeof(wall), "z%05dxx", in_root);
	CHECK_MSG(last_err == -ENOSPC, "reuse: filling the root ended with %d, expected -ENOSPC",
		  last_err);
	/* 2. d, until $MFT has no record left. */
	in_d = fill_names(d, "n%05dxx", payload, &last_err);
	CHECK_MSG(in_d > 0, "reuse: no MFT record was left after the root's wall");
	CHECK_MSG(last_err == -ENOSPC, "reuse: filling d ended with %d, expected -ENOSPC",
		  last_err);
	/* 3. Free one record, K. */
	snprintf(name, sizeof(name), "n%05dxx", in_d - 1);
	err = ntfs_lookup(d, name, &f);
	if (!err) {
		memset(&at, 0, sizeof(at));
		err = ntfs_getattr(f, &at);
		k = at.inode_no;
		ntfs_inode_put(f);
	}
	if (!err)
		err = ntfs_unlink(d, name);
	CHECK_MSG(err == 0 && k, "reuse: freeing %s: %d", name, err);
	if (!err)
		in_d--;
	/* Every record has been handed out, so $MFT's size is its allocation. */
	memset(&info, 0, sizeof(info));
	err = ntfs_volume_get_info(vol, &info);
	CHECK_MSG(err == 0 && info.total_mft_records % 8 != 0 &&
		  k < (info.total_mft_records & ~7ull),
		  "reuse: get_info %d, $MFT has %" PRIu64 " records and K is %" PRIu64
		  "; the probe below reaches finding 28 only if the count is not a "
		  "multiple of 8 and K is below the byte holding the last records",
		  err, info.total_mft_records, k);
	err = ntfs_create(d, "probe", 0100644, &f);
	memset(&at, 0, sizeof(at));
	if (!err) {
		CHECK(ntfs_getattr(f, &at) == 0);
		ntfs_inode_put(f);
		CHECK(ntfs_unlink(d, "probe") == 0);
	}
	CHECK_MSG(err == 0 && at.inode_no == k,
		  "reuse: after freeing record %" PRIu64 ", create d/probe: %d, record %" PRIu64
		  " (finding 28)", k, err, at.inode_no);
	/* 4. The failing name again: it takes K and cannot split the root's index. */
	err = ntfs_create(root, wall, 0100644, &f);
	if (!err)
		ntfs_inode_put(f);
	CHECK_MSG(err == -ENOSPC, "reuse: %s at the root's full index: %d, expected -ENOSPC",
		  wall, err);
	/* 5. The next file must be given K. */
	err = ntfs_create(d, "victim", 0100644, &f);
	CHECK_MSG(err == 0, "reuse: create d/victim with record %" PRIu64 " free: %d", k, err);
	if (!err) {
		memset(&at, 0, sizeof(at));
		CHECK(ntfs_getattr(f, &at) == 0);
		got = at.inode_no;
		CHECK_MSG(got == k,
			  "reuse: d/victim got record %" PRIu64 ", not the failed create's %" PRIu64
			  "; the test no longer reaches finding 27", got, k);
		CHECK(ntfs_write(f, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload));
		ntfs_inode_put(f);
	}
	printf("  reuse: %d names in the root, %d in d; d/victim in record %" PRIu64
	       ", the failed create's %" PRIu64 "\n", in_root, in_d, got, k);
	ntfs_inode_put(d);
unmount:
	ntfs_inode_put(root);
	CHECK(ntfs_volume_sync(vol) == 0);
	CHECK(ntfs_unmount(vol) == 0);
	ntfs_bdev_close(dev);
	check_reuse_image("reuse");

	/* What reached the disk, not what was cached. */
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (!err) {
		CHECK(ntfs_volume_root(vol, &root) == 0);
		check_names("reuse: root", root, "z%05dxx", in_root, NULL);
		if (wall[0]) {
			err = ntfs_lookup(root, wall, &f);
			if (!err)
				ntfs_inode_put(f);
			CHECK_MSG(err == -ENOENT,
				  "reuse: %s, whose create failed, after a remount: %d", wall, err);
		}
		err = ntfs_lookup(root, "d", &d);
		CHECK_MSG(err == 0, "reuse: d gone after a remount: %d", err);
		if (!err) {
			check_names("reuse: d", d, "n%05dxx", in_d, payload);
			err = ntfs_lookup(d, "probe", &f);
			if (!err)
				ntfs_inode_put(f);
			CHECK_MSG(err == -ENOENT, "reuse: d/probe, unlinked, after a remount: %d", err);
			err = ntfs_lookup(d, "victim", &f);
			memset(rbuf, 0, sizeof(rbuf));
			if (!err) {
				if (ntfs_read(f, rbuf, sizeof(rbuf), 0) != (ssize_t)sizeof(rbuf))
					err = -EIO;
				ntfs_inode_put(f);
			}
			CHECK_MSG(err == 0 && !memcmp(rbuf, payload, sizeof(payload)),
				  "reuse: d/victim, in the failed create's record, after a remount: %d%s",
				  err, err ? "" : ", other bytes");
			ntfs_inode_put(d);
		}
		ntfs_inode_put(root);
		ntfs_unmount(vol);
		ntfs_bdev_close(dev);
	}
	drop_image();
	printf("test_failed_create_record_reuse\n");
}

/*
 * U20 in docs/UPSTREAM-BUGS.md: ntfs_attr_map_cluster() when the update of
 * the mapping pairs fails after the allocator has succeeded. v7.1 freed the
 * new clusters in $Bitmap and left them in the run list, so the file went on
 * mapping clusters it no longer owned: they read back what was on the disk,
 * and the next mapping-pairs update would write them into the attribute, over
 * clusters another file can allocate. Mainline (91709ba5d6d7) restores the run
 * list from a copy and writes the mapping pairs again.
 *
 * The update fails here as it does when an MFT record cannot be had. The base
 * record of full.bin is full: clusters at 0 and HOLE_END, and a resident
 * stream ("fill") as large as the record holds. Single clusters then go into
 * the hole, every other one, each adding runs, until the mapping pairs
 * outgrow the room they have and need an attribute list, which needs an
 * extent record. Four files made after full.bin put the free record that the
 * allocator picks in another page of $MFT than full.bin's, and on a fresh
 * mount nothing has read that page: the device refuses every read during the
 * writes, so the record cannot be mapped. With clusters of a page or more the
 * writes before it read nothing, and everything the allocation needs is
 * cached by then. With smaller clusters the first write would fail reading
 * its own page instead, so the refused read has to be one of $MFT's.
 *
 * The case reaches neither U20's free of a second run (it allocates one
 * cluster at a time) nor its second update (the first fails inside
 * ntfs_inode_add_attrlist(), before any mapping pairs are written): the
 * review checked those with failure hooks in a scratch copy.
 *
 * The free clusters hold 'D' first. Without the fix, the run list still maps
 * the cluster of the write that failed, and a read on the same mount returns
 * 'D'. ntfs_error() takes the volume read-only meanwhile (errors=remount-ro),
 * so nothing reaches the disk and a remount reads the hole either way: the
 * same-mount read is the evidence.
 */
#define FILL_XATTR "fill"
#define HOLE_END 64	/* full.bin's hole is [1, HOLE_END) */

static void test_failed_mapping_pairs_update(void)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *f;
	struct ntfs_volume_info info;
	uint8_t *buf = NULL;
	uint64_t cs, lo, hi, before, r, vcn = 0;
	char name[16];
	size_t stale = 0, nonzero = 0;
	ssize_t wn = 0;
	int err;

	if (make_image("mapfail", 8) != 0)
		return;
	fault_clear();
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err) { drop_image(); return; }
	CHECK(ntfs_volume_root(vol, &root) == 0);
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	cs = info.cluster_size;
	buf = malloc(cs > 4096 ? cs : 4096);
	err = buf ? ntfs_create(root, "full.bin", 0100644, &f) : -ENOMEM;
	CHECK(err == 0);
	if (err) {
		ntfs_inode_put(root);
		ntfs_unmount(vol);
		ntfs_bdev_close(dev);
		goto out;
	}
	memset(buf, 'F', cs);
	CHECK(ntfs_write(f, buf, cs, 0) == (ssize_t)cs);
	CHECK(ntfs_write(f, buf, cs, HOLE_END * cs) == (ssize_t)cs);
	CHECK(ntfs_fsync(f, false) == 0);
	/* The largest resident "fill": a size is resident if it costs no
	 * cluster. */
	memset(buf, 'x', 4096);
	for (lo = 0, hi = info.mft_record_size; lo + 1 < hi; ) {
		uint64_t mid = (lo + hi) / 2;

		CHECK(ntfs_volume_get_info(vol, &info) == 0);
		before = info.free_clusters;
		err = ntfs_setxattr(f, FILL_XATTR, buf, mid, 0);
		CHECK(ntfs_volume_get_info(vol, &info) == 0);
		if (!err && info.free_clusters == before)
			lo = mid;
		else
			hi = mid;
		ntfs_removexattr(f, FILL_XATTR);
	}
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	before = info.free_clusters;
	CHECK_MSG(lo > 0 && ntfs_setxattr(f, FILL_XATTR, buf, lo, 0) == 0,
		  "the record of full.bin could not be filled (%" PRIu64 ")", lo);
	CHECK(ntfs_volume_get_info(vol, &info) == 0);
	CHECK_MSG(info.free_clusters == before, "\"fill\" did not stay resident");
	CHECK(ntfs_fsync(f, false) == 0);
	ntfs_inode_put(f);
	for (int i = 0; i < 4; i++) {
		snprintf(name, sizeof(name), "n%d", i);
		CHECK(ntfs_create(root, name, 0100644, &f) == 0);
		ntfs_inode_put(f);
	}
	CHECK(stain_free_space(vol, root, 'D', cs) == 0);
	ntfs_inode_put(root);
	CHECK(ntfs_volume_sync(vol) == 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	/* The writes, with every uncached page out of reach. */
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err)
		goto out;
	CHECK(ntfs_volume_root(vol, &root) == 0);
	err = ntfs_lookup(root, "full.bin", &f);
	CHECK(err == 0);
	if (!err) {
		memset(buf, 'W', cs);
		fi.mode = FAULT_READ;
		for (vcn = 2; vcn < HOLE_END - 1; vcn += 2) {
			wn = ntfs_write(f, buf, cs, vcn * cs);
			if (wn != (ssize_t)cs)
				break;
		}
		printf("  failed_mapping_pairs_update: \"fill\" %" PRIu64 " bytes, the "
		       "write at cluster %" PRIu64 " returned %zd, %d read(s) refused\n",
		       lo, vcn, wn, fi.fired);
		CHECK_MSG(wn < 0, "every write into full.bin's hole found room for its "
			  "mapping pairs, so this case tested nothing");
		CHECK_MSG(fi.fired > 0, "no read was refused during the writes: this case "
			  "tested nothing");
		{
			struct geom g;
			bool in_mft = false;

			if (read_geom(&g) == 0 && ntfs_volume_get_info(vol, &info) == 0)
				in_mft = fi.fired_off >= g.mft_off &&
					 fi.fired_off < g.mft_off + info.total_mft_records *
					 (uint64_t)info.mft_record_size;
			CHECK_MSG(in_mft, "the read refused during the writes, at byte %" PRIu64
				  ", is not $MFT's: the write failed before its mapping pairs, "
				  "so this case tested nothing", fi.fired_off);
		}
		fault_clear();
		CHECK(ntfs_read(f, buf, cs, vcn * cs) == (ssize_t)cs);
		for (uint64_t b = 0; b < cs; b++) {
			nonzero += buf[b] != 0;
			stale += buf[b] == 'D';
		}
		CHECK_MSG(nonzero == 0, "after the failed write, the hole of full.bin "
			  "reads %zu non-zero bytes on the same mount, %zu of them a deleted "
			  "file's 'D'", nonzero, stale);
		CHECK_MSG(ntfs_seek_data_hole(f, vcn * cs, false, &r) == 0 &&
			  r == HOLE_END * cs,
			  "full.bin maps a cluster at %" PRIu64 " after the failed write", vcn);
		ntfs_inode_put(f);
	}
	fault_clear();
	ntfs_inode_put(root);
	ntfs_volume_sync(vol);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	/* After a remount: the hole, and a clean volume. */
	err = mount_rw(&dev, &vol);
	CHECK(err == 0);
	if (err)
		goto out;
	CHECK(ntfs_volume_root(vol, &root) == 0);
	err = ntfs_lookup(root, "full.bin", &f);
	CHECK(err == 0);
	if (!err) {
		CHECK(ntfs_read(f, buf, cs, vcn * cs) == (ssize_t)cs);
		for (r = 0; r < cs && !buf[r]; r++)
			;
		CHECK_MSG(r == cs, "after a remount, cluster %" PRIu64 " of full.bin is not "
			  "zero", vcn);
		ntfs_inode_put(f);
	}
	ntfs_inode_put(root);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	check_image("failed_mapping_pairs_update");
out:
	free(buf);
	drop_image();
	printf("test_failed_mapping_pairs_update\n");
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
	test_write_fail_into_hole("holefail", NULL);
	test_write_fail_into_hole("holefail512", "-c 512");
	test_flush_fail();
	test_enospc();
	test_enospc_names();
	test_failed_create_record_reuse();
	test_failed_mapping_pairs_update();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
