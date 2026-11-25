// SPDX-License-Identifier: GPL-2.0
/*
 * test_compress.c - writing to compressed files, and the sparse paths.
 *
 * core/ntfs/compress.c is 1,550 lines and until now only its read half had
 * ever run: ntfs_compress_block(), ntfs_write_cb() and ntfs_compress_write()
 * -- the LZ77 encoder, the compression-block rewrite, the punch-hole and
 * cluster-reallocation that go with it -- had no test at all, and the
 * compressed-sparse fixture was only ever read. That is the largest piece of
 * implemented-but-unverified code in the project, and it is the piece that
 * rewrites a user's file in place.
 *
 * What each group covers, and the risk it addresses:
 *
 *   overwrite_in_place   The encoder produces a different compressed length
 *                        every time it runs, so every in-place write frees the
 *                        block's old clusters and allocates new ones. A wrong
 *                        length, a lost run or a merge that drops an extent
 *                        corrupts the file and the volume's cluster bitmap.
 *                        Compressible, incompressible and unaligned-tail files
 *                        are all covered, because the encoder takes a different
 *                        branch for each (compressed, stored raw, short block).
 *   cb_boundary          A write that straddles a 64 KiB compression block
 *                        touches two independent rewrites; one that covers
 *                        several whole blocks runs the loop. Both re-read
 *                        surrounding data out of the page cache, so a bad
 *                        offset silently overwrites neighbouring bytes with
 *                        zeroes rather than failing.
 *   extend               Writing past EOF, both inside the allocation rounding
 *                        and past it. Two data-loss bugs lived here; see below.
 *   append               The same thing the way a caller does it: repeated
 *                        writes at EOF in a chunk size that is no factor of the
 *                        compression block, compared only after a remount.
 *   zero_block           Writing zeroes over a block that holds data, and over
 *                        one that is already a hole. One data-loss bug lived
 *                        here; see below.
 *   size_change_refused  core/vfs/file.c refuses truncate and fallocate on a
 *                        compressed or encrypted inode. Pinned so it cannot
 *                        start half-working without anyone noticing.
 *   resident_grow        A compressed file small enough to be resident does
 *                        NOT go through compress.c when it grows; it must
 *                        still end up readable.
 *   sparse               Holes read as zeroes, allocated size stays below data
 *                        size, and the data/hole boundaries agree with the run
 *                        list.
 *   encrypted_refused    Every data entry point must refuse an encrypted
 *                        inode, in both directions.
 *   fragmented_rewrite   A block rewritten where only short free runs are
 *                        left must be written to each run it was given.
 *   full_volume_rewrite  A rewrite with no room for the new block must fail
 *                        and leave the old one, on the disk and in the cache,
 *                        even when the old block's clusters could hold it.
 *   rewrite_write_error  The same when the device refuses the block's write,
 *                        after its new clusters were allocated.
 *   *_update_fails       The same when the new mapping pairs cannot be
 *                        written: the record must describe the old block
 *                        again, sizes included.
 *
 * Three bugs were found while writing this, and between them they meant that
 * appending to a compressed file did not work at all. All three exist verbatim
 * in Linux 7.1's fs/ntfs/compress.c, all three reported success to the caller,
 * and all three left the volume ntfsck-clean, so nothing noticed. They are
 * findings 9, 10 and 11 in docs/UPSTREAM-BUGS.md; the assertions that used to
 * pin them now assert the fixed behaviour.
 *
 *   1. ntfs_compress_write() grew the attribute only when the write passed
 *      ni->allocated_size, not ni->data_size. A compressed file's allocation is
 *      rounded up to the compression block, so a write into that rounding gap
 *      returned success, rewrote the block, and left i_size alone: the bytes
 *      were on disk and unreachable. Fixed by testing data_size, which is where
 *      EOF actually is.
 *   2. Nothing on the compressed write path advanced initialized_size. When the
 *      write did pass allocated_size, ntfs_attr_expand() moved data_size but
 *      initialized_size stayed at the old EOF, so everything written past the
 *      old end of file read back as zeroes -- after a remount too. Fixed by
 *      advancing it once the data is on the disk, the shape core/vfs/api.c's
 *      do_write() already used for uncompressed writes.
 *   3. ntfs_write_cb() returned early when a block compressed to nothing but
 *      zeroes, before it punched the old block's clusters out. Zeroing a range
 *      that held data left the old data on disk and readable. Fixed by punching
 *      the block when it holds clusters; the shortcut is kept for the case it
 *      was written for, a block that is already a hole.
 *
 * All three were data loss of the quiet kind, so each is held down here from
 * both ends: what the write reports, and what a remount reads back.
 *
 * Every write cycle is followed by ntfsck -n (ntfsprogs-plus), which is the
 * only check that sees the cluster bitmap and the mapping pairs. -n always:
 * repairing the image under test would measure the repair.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ntfscore.h"
#include "ntfsport/bdev.h"
#include "ntfs_image.h"
#include "ntfs_logfile.h"

static int failures, checks;

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

#define CHECK_EQ(got, want, what) do {					\
	long long _g = (long long)(got), _w = (long long)(want);	\
	checks++;							\
	if (_g != _w) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s = %lld, expected %lld\n",\
			__FILE__, __LINE__, (what), _g, _w);		\
	}								\
} while (0)

/* The volume's compression block: 16 clusters of 4 KiB in every fixture that
 * carries compressed data. Hard-coded rather than derived, so a fixture whose
 * geometry changed shows up as a failure instead of quietly testing nothing. */
#define CB_SIZE 65536u

static const char *images_dir(void)
{
	const char *d = getenv("NTFS_IMAGES");
	return d && *d ? d : "tools/images";
}

/* ---- scratch copies -------------------------------------------------- */

static char scratch[1024];

static int copy_fixture(const char *name)
{
	char src[1024], buf[1 << 16];
	int in, out;
	ssize_t n;

	snprintf(src, sizeof(src), "%s/%s", images_dir(), name);
	snprintf(scratch, sizeof(scratch), "/tmp/ttntfs-compress-%d-%s", (int)getpid(), name);
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

/*
 * ntfsck -n on the scratch image. This is what catches the damage a read-back
 * comparison cannot see: freed clusters still marked in use, mapping pairs that
 * no longer describe the allocation. It does not compare a compressed size with
 * the run list (U22), so the cases that can break that check it themselves.
 * Never without -n.
 */
static void fsck(const char *stage)
{
	const char *ck = getenv("NTFSCK");
	char cmd[2048];
	int rc;

	if (!ck || !*ck || access(ck, X_OK) != 0) {
		static int warned;
		if (!warned++)
			fprintf(stderr, "SKIP ntfsck: $NTFSCK not usable\n");
		return;
	}
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' >/dev/null 2>&1", ck, scratch);
	rc = system(cmd);
	checks++;
	if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
		failures++;
		fprintf(stderr, "FAIL ntfsck after %s: rc=%d (exit %d)\n",
			stage, rc, WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
	}
}

/* ---- mount helpers --------------------------------------------------- */

static struct ntfs_bdev *g_dev;
static ntfs_volume_t *g_vol;

/*
 * A device that refuses some transfers, for the cases that need one to fail.
 * While g_refusing is set, mount_scratch() splices refuse_pwrite() and
 * refuse_pread() in front of the image's ops, as test_faults.c does. Every
 * write that starts with the first 4 KiB of refuse_data fails with -EIO: a
 * case picks out the one write it wants refused by what that write carries.
 * Every read that touches [refuse_read_lo, refuse_read_hi) fails with -EIO
 * while that range is set, and the first such read's offset is kept.
 */
static bool g_refusing;
static const unsigned char *refuse_data;
static int refused;
static uint64_t refuse_read_lo, refuse_read_hi, refused_read_off;
static int refused_reads;
static const struct ntfs_bdev_ops *real_ops;
static struct ntfs_bdev_ops refuse_ops;

static ssize_t refuse_pwrite(struct ntfs_bdev *d, const void *b, size_t c, u64 o)
{
	if (refuse_data && c >= 4096 && !memcmp(b, refuse_data, 4096)) {
		refused++;
		return -EIO;
	}
	return real_ops->pwrite(d, b, c, o);
}

static ssize_t refuse_pread(struct ntfs_bdev *d, void *b, size_t c, u64 o)
{
	if (o < refuse_read_hi && o + c > refuse_read_lo) {
		if (!refused_reads++)
			refused_read_off = o;
		return -EIO;
	}
	return real_ops->pread(d, b, c, o);
}

/*
 * Mount the scratch image. A read-only mount fails the test, since nothing
 * below would be exercised, except where @after_error says that the last
 * mount ended in an error that took the volume read-only. The unmount after
 * such an error may leave the volume marked dirty, as finding 33's does, and a
 * dirty volume mounts read-only only: the fallback is asked for then, and a
 * read-only mount is accepted for that reason alone.
 */
static int mount_scratch_after(bool after_error)
{
	struct ntfs_mount_options o = { .flags = 0, .uid = 0, .gid = 0,
					.fmask = 022, .dmask = 022 };
	struct ntfs_volume_info info;
	int err;

	if (after_error)
		o.flags = NTFS_MOUNT_RDONLY_FALLBACK;
	g_dev = ntfs_bdev_open_path(scratch, false);
	if (!g_dev)
		return -ENOENT;
	if (g_refusing) {
		real_ops = g_dev->ops;
		refuse_ops = *real_ops;
		refuse_ops.pwrite = refuse_pwrite;
		refuse_ops.pread = refuse_pread;
		g_dev->ops = &refuse_ops;
	}
	err = ntfs_mount(g_dev, &o, &g_vol);
	if (err) {
		ntfs_bdev_close(g_dev);
		g_dev = NULL;
		return err;
	}
	ntfs_volume_get_info(g_vol, &info);
	if (info.read_only && !(after_error && info.ro_reason == NTFS_RO_DIRTY)) {
		fprintf(stderr, "FAIL: fixture mounted read-only (reason %d); "
			"nothing below would have been exercised\n", info.ro_reason);
		failures++; checks++;
	}
	return 0;
}

static int mount_scratch(void)
{
	return mount_scratch_after(false);
}

static void umount_scratch(void)
{
	if (g_vol)
		ntfs_unmount(g_vol);
	if (g_dev)
		ntfs_bdev_close(g_dev);
	g_vol = NULL;
	g_dev = NULL;
}

/* Two-component lookup: the fixture's compressed and sparse files all live one
 * directory below the root. */
static ntfs_inode_t *open_file(const char *dir, const char *name)
{
	ntfs_inode_t *root, *d, *f = NULL;

	if (ntfs_volume_root(g_vol, &root))
		return NULL;
	if (ntfs_lookup(root, dir, &d)) {
		ntfs_inode_put(root);
		return NULL;
	}
	if (ntfs_lookup(d, name, &f))
		f = NULL;
	ntfs_inode_put(d);
	ntfs_inode_put(root);
	return f;
}

static uint64_t data_size(ntfs_inode_t *ni)
{
	struct ntfs_attr a;

	if (ntfs_getattr(ni, &a))
		return UINT64_MAX;
	return a.size;
}

/* For a compressed inode ntfs_getattr reports the COMPRESSED size here, not the
 * allocation (core/vfs/api.c fill_attr). Named accordingly so no test reads it
 * as "clusters allocated". */
static uint64_t on_disk_size(ntfs_inode_t *ni)
{
	struct ntfs_attr a;

	if (ntfs_getattr(ni, &a))
		return UINT64_MAX;
	return a.alloc_size;
}

static int read_exact(ntfs_inode_t *ni, void *buf, size_t len, uint64_t off)
{
	ssize_t r = ntfs_read(ni, buf, len, off);

	return r == (ssize_t)len ? 0 : (r < 0 ? (int)r : -EIO);
}

/* Deterministic incompressible bytes: the encoder must fall back to storing the
 * block raw, which is a different branch of ntfs_write_cb() from the one a
 * compressible block takes. */
static void fill_random(unsigned char *p, size_t n, unsigned seed)
{
	unsigned s = seed | 1;

	while (n--) {
		s = s * 1103515245u + 12345u;
		*p++ = (unsigned char)(s >> 16);
	}
}

/* A whole buffer of zeroes, which is what every read of a hole must produce. */
static int all_zero(const unsigned char *p, size_t n)
{
	while (n--)
		if (*p++)
			return 0;
	return 1;
}

/* First differing byte, or -1. */
static long first_diff(const unsigned char *a, const unsigned char *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (a[i] != b[i])
			return (long)i;
	return -1;
}

static void compare(const char *what, const unsigned char *got,
		    const unsigned char *want, size_t n)
{
	long d = first_diff(got, want, n);

	checks++;
	if (d >= 0) {
		failures++;
		fprintf(stderr, "FAIL %s: content differs at byte %ld "
			"(got 0x%02x, expected 0x%02x) of %zu\n",
			what, d, got[d], want[d], n);
	}
}

/* ---- 1. overwrite in place ------------------------------------------- */

/*
 * The common case and the dangerous one: every write to a compressed file
 * re-encodes the whole 64 KiB block, frees its clusters and allocates a new,
 * differently sized run. Nothing outside the written range may move.
 */
/*
 * Creating files in a Windows-compressed DIRECTORY must work, and must not take
 * the volume read-only.
 *
 * Inode load sets NInoCompressed on a directory whose $INDEX_ROOT carries the
 * compression flag, and documents it as an inheritance marker: children should
 * be created compressed. ntfs_attr_truncate() read the same flag as "this
 * attribute is compressed data" and refused with EOPNOTSUPP. So the first
 * create that had to grow the directory's $INDEX_ROOT failed inside
 * ntfs_ir_truncate(), which reported it through ntfs_error(), and the
 * errors=remount-ro policy took the whole volume read-only. One `echo > file`
 * in a folder Windows had marked compressed: volume read-only. Measured on
 * 2026-09-18; the directory's own attributes are never compressed data.
 *
 * Enough files with long names that the index root has to grow. The
 * read-only check is the one that matters: a refused create is an
 * inconvenience, a volume that silently goes read-only is not.
 */
static void test_create_in_compressed_directory(void)
{
	ntfs_inode_t *root = NULL, *comp = NULL, *dir = NULL, *f = NULL;
	struct ntfs_volume_info info;
	int i, rc = 0;

	printf("test_create_in_compressed_directory\n");
	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_create_in_compressed_directory: no fixture\n");
		return;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	if (ntfs_volume_root(g_vol, &root) || ntfs_lookup(root, "compressed", &comp) ||
	    ntfs_lookup(comp, "inherit", &dir)) {
		fprintf(stderr, "SKIP: /compressed/inherit not in fixture\n");
		goto out;
	}
	/*
	 * Real-sized files, not one-byte ones. The first version of this test
	 * wrote a single byte each and passed while the same sequence with
	 * 200 KB files took a real volume read-only: growing the directory's
	 * $INDEX_ROOT ran ntfs_resident_attr_resize(), which saw NInoCompressed
	 * on the directory and stored the new index-root size through the
	 * compressed side of the itype union -- over itype.index.block_size.
	 * The next lookup compared an INDX block's 4096 against 480 and called
	 * the directory corrupt. Non-resident children are what make the index
	 * grow in the pattern that reached it.
	 */
	{
		static char big[200000];

		memset(big, 'Q', sizeof(big));
		/* Short names on purpose: they make the index root grow in several
		 * small resident resizes before it overflows, and that is the
		 * sequence that reached the clobber. With long names the root
		 * overflowed at once and the test passed with the bug present. */
		for (i = 0; i < 8 && !rc; i++) {
			char name[32];

			snprintf(name, sizeof(name), "c%d-file.bin", i);
			rc = ntfs_create(dir, name, 0100644, &f);
			if (!rc) {
				CHECK_EQ(ntfs_write(f, big, sizeof(big), 0), (ssize_t)sizeof(big),
					 "200 KB write into a file in a compressed dir");
				ntfs_inode_put(f); f = NULL;
			}
		}
	}
	CHECK_EQ(rc, 0, "create in a compressed directory (was EOPNOTSUPP from "
			"ntfs_attr_truncate on the directory's own $INDEX_ROOT)");
	ntfs_volume_get_info(g_vol, &info);
	checks++;
	if (info.read_only) {
		failures++;
		fprintf(stderr, "FAIL: the volume went READ-ONLY after creating a file in a "
				"compressed directory. That is the errors=remount-ro policy firing "
				"on a code bug, and it is what a user would see.\n");
	}

	/*
	 * The files must read back intact, through our own reader, after a
	 * remount. This is the guard against enabling compression inheritance
	 * (NV_Compression in core/ntfs/super.c) before the fresh-file compressed
	 * writer is fixed: with the flag on, these files report their full size
	 * and return EIO -- measured 2026-09-18. Today they are uncompressed and
	 * correct, which is the intended state until that writer works.
	 *
	 * And a subdirectory made here must carry the compression marker on, so
	 * Windows compresses what it creates inside it.
	 */
	{
		ntfs_inode_t *sub = NULL;

		CHECK_EQ(ntfs_mkdir(dir, "inherit-sub", 0755, &sub), 0, "mkdir in compressed dir");
		if (sub) {
			struct ntfs_attr sa;

			CHECK(ntfs_getattr(sub, &sa) == 0);
			CHECK_EQ(sa.compressed, 1, "a subdirectory of a compressed folder must carry "
						 "the compression marker (Windows semantics)");
			ntfs_inode_put(sub);
		}
	}
	if (dir) { ntfs_inode_put(dir); dir = NULL; }
	if (comp) { ntfs_inode_put(comp); comp = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	umount_scratch();
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	if (!ntfs_volume_root(g_vol, &root) && !ntfs_lookup(root, "compressed", &comp) &&
	    !ntfs_lookup(comp, "inherit", &dir)) {
		static char big[200000], back[200000];

		memset(big, 'Q', sizeof(big));
		for (i = 0; i < 8; i++) {
			char name[32];
			struct ntfs_attr fa;
			ssize_t n;

			snprintf(name, sizeof(name), "c%d-file.bin", i);
			if (ntfs_lookup(dir, name, &f)) { failures++; checks++; continue; }
			CHECK(ntfs_getattr(f, &fa) == 0);
			CHECK_EQ(fa.size, (int64_t)sizeof(big), "size after remount");
			n = ntfs_read(f, back, sizeof(back), 0);
			CHECK_EQ(n, (ssize_t)sizeof(big), "read after remount -- EIO here is what "
				 "enabling NV_Compression without fixing the fresh-file writer causes");
			if (n == (ssize_t)sizeof(big))
				CHECK_EQ(memcmp(back, big, sizeof(big)), 0, "content after remount");
			ntfs_inode_put(f); f = NULL;
		}
	}
out:
	if (dir) ntfs_inode_put(dir);
	if (comp) ntfs_inode_put(comp);
	if (root) ntfs_inode_put(root);
	umount_scratch();
	fsck("create in compressed directory");
	unlink(scratch);
}

static void test_overwrite_in_place(void)
{
	static const char *files[] = {
		"text-100k.txt",	/* compressible: the encoder emits compressed blocks */
		"random-1m.bin",	/* incompressible: every block is stored raw */
		"odd-size.txt",		/* 123457 bytes: a short, unaligned last block */
		"one-cu-plus-1.bin",	/* 65537 bytes: a second block holding one byte */
	};

	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		unsigned char *ref, *back, patch[4096];
		ntfs_inode_t *ni;
		uint64_t size;

		if (copy_fixture("compressed-sparse.img") != 0) {
			fprintf(stderr, "SKIP test_overwrite_in_place: no fixture\n");
			return;
		}
		if (mount_scratch()) {
			fprintf(stderr, "FAIL: cannot mount fixture\n");
			failures++; checks++; unlink(scratch); return;
		}
		ni = open_file("compressed", files[i]);
		CHECK(ni != NULL);
		if (!ni) { umount_scratch(); unlink(scratch); continue; }

		{
			struct ntfs_attr a;
			CHECK(ntfs_getattr(ni, &a) == 0);
			CHECK(a.compressed);		/* else this tests nothing */
			size = a.size;
		}
		ref = malloc(size);
		back = malloc(size);
		CHECK(ref && back);
		if (!ref || !back) { free(ref); free(back); ntfs_inode_put(ni);
				     umount_scratch(); unlink(scratch); continue; }
		CHECK_EQ(read_exact(ni, ref, size, 0), 0, files[i]);

		/* Two overwrites, one at the very start and one at a block-interior
		 * offset that is not page aligned, so the partial-page copy inside
		 * ntfs_compress_write() is exercised as well. */
		fill_random(patch, sizeof(patch), 0x51ee7 + (unsigned)i);
		CHECK_EQ(ntfs_write(ni, patch, sizeof(patch), 0), sizeof(patch),
			 "write at 0");
		memcpy(ref, patch, sizeof(patch));

		if (size > 20000) {
			memset(patch, 'A', sizeof(patch));
			CHECK_EQ(ntfs_write(ni, patch, 3333, 12345), 3333,
				 "unaligned interior write");
			memcpy(ref + 12345, patch, 3333);
		}

		/* An overwrite must never change the file's length. */
		CHECK_EQ(data_size(ni), size, "size after overwrite");

		/* Before the remount: what the page cache says. */
		CHECK_EQ(read_exact(ni, back, size, 0), 0, "read back (cached)");
		compare(files[i], back, ref, size);

		CHECK_EQ(ntfs_fsync(ni, false), 0, "fsync");
		CHECK_EQ(ntfs_volume_sync(g_vol), 0, "volume sync");
		ntfs_inode_put(ni);
		umount_scratch();
		fsck(files[i]);

		/* After the remount: what the encoder actually put on the disk,
		 * decoded by the other half of compress.c. */
		CHECK_EQ(mount_scratch(), 0, "remount");
		ni = open_file("compressed", files[i]);
		CHECK(ni != NULL);
		if (ni) {
			CHECK_EQ(data_size(ni), size, "size after remount");
			memset(back, 0, size);
			CHECK_EQ(read_exact(ni, back, size, 0), 0, "read back (on disk)");
			compare(files[i], back, ref, size);
			ntfs_inode_put(ni);
		}
		umount_scratch();
		free(ref);
		free(back);
		unlink(scratch);
	}
}

/* ---- 2. compression block boundaries --------------------------------- */

/*
 * ntfs_compress_write() walks the write one compression block at a time and
 * re-reads the rest of each block through the page cache. An off-by-one in the
 * block offset or the per-page copy does not fail: it writes zeroes over the
 * bytes on the other side of the boundary. Only a full-file comparison sees it.
 */
static void test_cb_boundary(void)
{
	unsigned char *ref, *back, *patch;
	ntfs_inode_t *ni;
	uint64_t size;

	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_cb_boundary: no fixture\n");
		return;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	ni = open_file("compressed", "text-100k.txt");
	CHECK(ni != NULL);
	if (!ni) { umount_scratch(); unlink(scratch); return; }

	size = data_size(ni);
	CHECK_EQ(size, 102400, "fixture size");	/* two compression blocks, the second short */
	ref = malloc(size);
	back = malloc(size);
	patch = malloc(size);
	CHECK(ref && back && patch);
	if (!ref || !back || !patch) goto out;
	CHECK_EQ(read_exact(ni, ref, size, 0), 0, "reference read");

	/* (a) straddle the block 0 / block 1 boundary at 65536. */
	fill_random(patch, 4000, 0xb0bu);
	CHECK_EQ(ntfs_write(ni, patch, 4000, CB_SIZE - 1000), 4000, "straddling write");
	memcpy(ref + CB_SIZE - 1000, patch, 4000);

	/* (b) one write covering both blocks end to end, staying inside the
	 * file so no size change is involved. */
	memset(patch, 0x5a, 100000);
	fill_random(patch + 40000, 20000, 0xfeed);	/* a raw stretch inside a compressible one */
	CHECK_EQ(ntfs_write(ni, patch, 100000, 1000), 100000, "multi-block write");
	memcpy(ref + 1000, patch, 100000);

	CHECK_EQ(data_size(ni), size, "size unchanged by interior writes");
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "volume sync");
	ntfs_inode_put(ni);
	umount_scratch();
	fsck("cb_boundary");

	CHECK_EQ(mount_scratch(), 0, "remount");
	ni = open_file("compressed", "text-100k.txt");
	CHECK(ni != NULL);
	if (ni) {
		CHECK_EQ(data_size(ni), size, "size after remount");
		CHECK_EQ(read_exact(ni, back, size, 0), 0, "read back");
		compare("text-100k.txt across block boundaries", back, ref, size);
		ntfs_inode_put(ni);
	}
out:
	free(ref); free(back); free(patch);
	umount_scratch();
	unlink(scratch);
}

/* ---- 3. extending a compressed file ---------------------------------- */

/*
 * Both halves of this test pin bugs. Writing past the end of a compressed file
 * reports success and loses the data, in two different ways depending on
 * whether the write stays inside the allocation.
 */
static void test_extend(void)
{
	unsigned char *ref, *back, *patch;
	ntfs_inode_t *ni;
	uint64_t size, grown;
	const uint64_t far = 1u << 20;	/* well past the 128 KiB allocation */

	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_extend: no fixture\n");
		return;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	ni = open_file("compressed", "text-100k.txt");
	CHECK(ni != NULL);
	if (!ni) { umount_scratch(); unlink(scratch); return; }

	size = data_size(ni);
	CHECK_EQ(size, 102400, "fixture size");
	ref = malloc(far + CB_SIZE);
	back = malloc(far + CB_SIZE);
	patch = malloc(CB_SIZE);
	CHECK(ref && back && patch);
	if (!ref || !back || !patch) goto out;
	memset(ref, 0, far + CB_SIZE);
	CHECK_EQ(read_exact(ni, ref, size, 0), 0, "reference read");

	/*
	 * (a) 102400 rounds up to a 131072-byte allocation, so 8 KiB at EOF
	 * still fits inside it. ntfs_compress_write() must grow i_size even
	 * though the write never passes allocated_size (finding 9): a write
	 * that landed inside the compression-block rounding used to report
	 * success, re-encode the block and leave i_size alone, which put the
	 * bytes on the disk and out of reach for ever after.
	 */
	memset(patch, 'Z', 8192);
	CHECK_EQ(ntfs_write(ni, patch, 8192, size), 8192, "write 8 KiB at EOF");
	CHECK_EQ(data_size(ni), size + 8192, "i_size grows for a write past EOF "
		 "that lands inside the compression-block rounding");
	memset(back, 0, 8192);
	CHECK_EQ(read_exact(ni, back, 8192, size), 0, "read back the extension");
	memset(ref + size, 'Z', 8192);
	compare("the 8 KiB written at EOF", back, ref + size, 8192);

	/* What was already in the file must have survived too. */
	CHECK_EQ(read_exact(ni, back, size, 0), 0, "read back after the extend");
	compare("existing data after an extend", back, ref, size);
	size += 8192;

	/*
	 * (b) past the allocation this time, so ntfs_attr_expand() allocates
	 * as well, and the gap between the old EOF and @far becomes a hole.
	 *
	 * The write must also advance initialized_size (finding 10). While it
	 * did not, initialized_size stayed at the old EOF and every byte past
	 * it -- the ones just written included -- read back as zero, before
	 * and after a remount. ntfsck was happy, because an attribute whose
	 * initialized_size is below its data_size is perfectly legal; it just
	 * does not mean what the writer intended.
	 */
	memset(patch, 'Q', 4096);
	CHECK_EQ(ntfs_write(ni, patch, 4096, far), 4096, "write past the allocation");
	grown = data_size(ni);
	CHECK_EQ(grown, far + 4096, "i_size grows when the write passes allocated_size");

	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "volume sync");
	ntfs_inode_put(ni);
	umount_scratch();
	fsck("extend");

	CHECK_EQ(mount_scratch(), 0, "remount");
	ni = open_file("compressed", "text-100k.txt");
	CHECK(ni != NULL);
	if (ni) {
		CHECK_EQ(data_size(ni), grown, "size after remount");
		/* Everything below the ORIGINAL EOF is still correct... */
		CHECK_EQ(read_exact(ni, back, size, 0), 0, "read back the old data");
		compare("data below the old EOF", back, ref, size);
		/* ...the gap left between the old EOF and the far write is a
		 * hole and reads as zeroes... */
		memset(back, 0xff, CB_SIZE);
		CHECK_EQ(read_exact(ni, back, CB_SIZE, far / 2), 0, "read back the gap");
		CHECK(all_zero(back, CB_SIZE));
		/* ...and the bytes written past the allocation are there. The
		 * compressed write path must advance initialized_size (finding
		 * 10); while it did not, everything past the old EOF -- the
		 * data just written included -- read back as a hole. */
		memset(back, 0xff, 4096);
		CHECK_EQ(read_exact(ni, back, 4096, far), 0, "read back the new data");
		compare("the 4 KiB written past the allocation", back, patch, 4096);
		ntfs_inode_put(ni);
	}
out:
	free(ref); free(back); free(patch);
	umount_scratch();
	unlink(scratch);
}

/*
 * Appending, the way a caller actually does it: repeated writes at EOF, each
 * one starting where the last finished. The chunk size is deliberately not a
 * factor of the 64 KiB compression block, so the run crosses block boundaries
 * at every offset within a block -- inside the allocation rounding, exactly on
 * it, and past it -- which is the ground findings 9 and 10 sat on. Nothing here
 * is checked from the page cache: the file is compared only after a remount.
 */
static void test_append(void)
{
	unsigned char *ref, *back, chunk[7000];
	ntfs_inode_t *ni;
	uint64_t size, grown;
	const int rounds = 40;		/* 280000 bytes: past the 128 KiB allocation */

	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_append: no fixture\n");
		return;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	ni = open_file("compressed", "text-100k.txt");
	CHECK(ni != NULL);
	if (!ni) { umount_scratch(); unlink(scratch); return; }

	size = data_size(ni);
	grown = size + (uint64_t)rounds * sizeof(chunk);
	ref = malloc(grown);
	back = malloc(grown);
	CHECK(ref && back);
	if (!ref || !back) goto out;
	CHECK_EQ(read_exact(ni, ref, size, 0), 0, "reference read");

	for (int i = 0; i < rounds; i++) {
		uint64_t at = size + (uint64_t)i * sizeof(chunk);

		fill_random(chunk, sizeof(chunk), 0xa99e0du + (unsigned)i);
		CHECK_EQ(ntfs_write(ni, chunk, sizeof(chunk), at), sizeof(chunk),
			 "append");
		CHECK_EQ(data_size(ni), at + sizeof(chunk), "i_size follows the append");
		memcpy(ref + at, chunk, sizeof(chunk));
	}

	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "volume sync");
	ntfs_inode_put(ni);
	umount_scratch();
	fsck("append");

	CHECK_EQ(mount_scratch(), 0, "remount");
	ni = open_file("compressed", "text-100k.txt");
	CHECK(ni != NULL);
	if (ni) {
		CHECK_EQ(data_size(ni), grown, "size after remount");
		memset(back, 0, grown);
		CHECK_EQ(read_exact(ni, back, grown, 0), 0, "read the whole file back");
		compare("a compressed file built by appending", back, ref, grown);
		ntfs_inode_put(ni);
	}
out:
	free(ref); free(back);
	umount_scratch();
	unlink(scratch);
}

/* ---- 4. writing zeroes over a block that holds data ------------------- */

/*
 * A compression block that encodes to nothing but zeroes is meant to become a
 * hole. ntfs_write_cb() detects that case and returns early -- before the
 * ntfs_non_resident_attr_punch_hole() call that would release the block's old
 * clusters. For a block that was already a hole that is correct and cheap. For
 * a block that held data it means the old data stays on the disk and stays
 * readable, and the caller is told the write succeeded.
 */
static void test_zero_block(void)
{
	unsigned char *ref, *back, *zeroes;
	ntfs_inode_t *ni;
	uint64_t size, before, after_first;

	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_zero_block: no fixture\n");
		return;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	ni = open_file("compressed", "text-100k.txt");
	CHECK(ni != NULL);
	if (!ni) { umount_scratch(); unlink(scratch); return; }

	size = data_size(ni);
	ref = malloc(size);
	back = malloc(size);
	zeroes = calloc(1, CB_SIZE);
	CHECK(ref && back && zeroes);
	if (!ref || !back || !zeroes) goto out;
	CHECK_EQ(read_exact(ni, ref, size, 0), 0, "reference read");
	CHECK(ref[0] != 0);		/* block 0 really does hold data */
	before = on_disk_size(ni);

	CHECK_EQ(ntfs_write(ni, zeroes, CB_SIZE, 0), CB_SIZE, "zero the first block");
	/* The block's clusters must actually have been released, not merely
	 * stopped being read: a punch-hole that did not happen leaves the
	 * compressed size where it was. */
	CHECK(on_disk_size(ni) < before);

	/*
	 * The same 64 KiB of zeroes again, over what is now a hole. This is
	 * the case the all-zeroes shortcut in ntfs_write_cb() was written for
	 * -- there is nothing to release -- and it must stay a cheap no-op
	 * that still reports the full count and still leaves the block a hole.
	 */
	after_first = on_disk_size(ni);
	CHECK_EQ(ntfs_write(ni, zeroes, CB_SIZE, 0), CB_SIZE,
		 "zero a block that is already a hole");
	CHECK_EQ(on_disk_size(ni), after_first, "re-zeroing a hole allocates nothing");

	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "volume sync");
	ntfs_inode_put(ni);
	umount_scratch();
	fsck("zero_block");

	CHECK_EQ(mount_scratch(), 0, "remount");
	ni = open_file("compressed", "text-100k.txt");
	CHECK(ni != NULL);
	if (ni) {
		CHECK_EQ(read_exact(ni, back, size, 0), 0, "read back");
		/*
		 * The zeroes that were written, not the block's previous
		 * contents. ntfs_write_cb() used to take its all-zeroes early
		 * return before punching the old block's clusters out (finding
		 * 11), so the old data stayed on the disk and stayed readable.
		 */
		CHECK_EQ(first_diff(back, zeroes, CB_SIZE), -1,
			 "64 KiB of zeroes written over a block holding data "
			 "must read back as zeroes");
		/* Everything past the zeroed block is untouched. */
		compare("data after the zeroed block", back + CB_SIZE,
			ref + CB_SIZE, (size_t)(size - CB_SIZE));
		ntfs_inode_put(ni);
	}
out:
	free(ref); free(back); free(zeroes);
	umount_scratch();
	unlink(scratch);
}

/* ---- 5. size changes are refused on compressed inodes ----------------- */

/*
 * core/vfs/file.c: ntfs_setattr_size() and ntfs_vfs_fallocate() both return
 * -EOPNOTSUPP for a compressed or encrypted inode. That is a refusal, not an
 * implementation, and it is what the FSKit layer sees; pinned so it cannot
 * start half-working. The refusal must also leave the file untouched -- a
 * refusal that has already truncated something is worse than no refusal.
 */
static void test_size_change_refused(void)
{
	unsigned char *ref, *back;
	ntfs_inode_t *ni;
	uint64_t size, compsz;

	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_size_change_refused: no fixture\n");
		return;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	ni = open_file("compressed", "text-100k.txt");
	CHECK(ni != NULL);
	if (!ni) { umount_scratch(); unlink(scratch); return; }

	size = data_size(ni);
	compsz = on_disk_size(ni);
	ref = malloc(size);
	back = malloc(size);
	CHECK(ref && back);
	if (!ref || !back) goto out;
	CHECK_EQ(read_exact(ni, ref, size, 0), 0, "reference read");

	CHECK_EQ(ntfs_truncate(ni, size / 2), -EOPNOTSUPP, "shrink refused");
	CHECK_EQ(ntfs_truncate(ni, size * 2), -EOPNOTSUPP, "grow refused");
	CHECK_EQ(ntfs_truncate(ni, 0), -EOPNOTSUPP, "truncate to zero refused");
	CHECK_EQ(ntfs_fallocate(ni, 0, CB_SIZE, true), -EOPNOTSUPP,
		 "fallocate keep_size refused");
	CHECK_EQ(ntfs_fallocate(ni, 0, CB_SIZE, false), -EOPNOTSUPP,
		 "fallocate refused");

	CHECK_EQ(data_size(ni), size, "size survived the refusals");
	CHECK_EQ(on_disk_size(ni), compsz, "allocation survived the refusals");
	CHECK_EQ(read_exact(ni, back, size, 0), 0, "read back");
	compare("content after refused size changes", back, ref, size);
out:
	free(ref); free(back);
	if (ni)
		ntfs_inode_put(ni);
	umount_scratch();
	fsck("size_change_refused");
	unlink(scratch);
}

/* ---- 6. a resident compressed file that grows ------------------------- */

/*
 * A compressed file small enough to live inside its MFT record has no
 * compression block at all, and core/vfs/api.c's do_write() only routes to
 * ntfs_compress_write() when the inode is already non-resident. So growing this
 * file runs the ordinary non-resident write path against an attribute that is
 * flagged compressed. That is legal on disk -- a compression unit whose whole
 * 16 clusters are allocated is stored raw, which is what comes out -- but it is
 * the one place where a compressed file is written by code that knows nothing
 * about compression, so it is worth holding still.
 */
static void test_resident_compressed_grow(void)
{
	unsigned char patch[8192], back[8192];
	ntfs_inode_t *ni;

	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_resident_compressed_grow: no fixture\n");
		return;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	ni = open_file("compressed", "small-resident.txt");
	CHECK(ni != NULL);
	if (!ni) { umount_scratch(); unlink(scratch); return; }

	{
		struct ntfs_attr a;
		CHECK(ntfs_getattr(ni, &a) == 0);
		CHECK(a.compressed);
		CHECK_EQ(a.size, 200, "fixture size");
	}

	fill_random(patch, sizeof(patch), 0x12e5);
	CHECK_EQ(ntfs_write(ni, patch, sizeof(patch), 0), sizeof(patch), "grow write");
	CHECK_EQ(data_size(ni), sizeof(patch), "size after growing a resident file");
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "volume sync");
	ntfs_inode_put(ni);
	umount_scratch();
	fsck("resident_compressed_grow");

	CHECK_EQ(mount_scratch(), 0, "remount");
	ni = open_file("compressed", "small-resident.txt");
	CHECK(ni != NULL);
	if (ni) {
		CHECK_EQ(data_size(ni), sizeof(patch), "size after remount");
		memset(back, 0, sizeof(back));
		CHECK_EQ(read_exact(ni, back, sizeof(back), 0), 0, "read back");
		compare("grown resident compressed file", back, patch, sizeof(back));
		ntfs_inode_put(ni);
	}
	umount_scratch();
	unlink(scratch);
}

/* ---- 7. sparse files -------------------------------------------------- */

static void test_sparse_fixtures(void)
{
	unsigned char *buf;
	ntfs_inode_t *ni;
	uint64_t r;

	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_sparse_fixtures: no fixture\n");
		return;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	buf = malloc(CB_SIZE);
	CHECK(buf != NULL);
	if (!buf) { umount_scratch(); unlink(scratch); return; }

	/* (a) a file that is nothing but hole. The allocation must be zero and
	 * every byte must read as zero -- decoding a hole is the one read path
	 * that produces data without touching the device. */
	ni = open_file("sparse", "all-hole-4m.bin");
	CHECK(ni != NULL);
	if (ni) {
		struct ntfs_attr a;
		CHECK(ntfs_getattr(ni, &a) == 0);
		CHECK(a.sparse);
		CHECK_EQ(a.size, 4194304, "all-hole size");
		CHECK_EQ(a.alloc_size, 0, "a file of pure hole allocates nothing");
		CHECK_EQ(read_exact(ni, buf, CB_SIZE, 0), 0, "read the hole");
		CHECK(all_zero(buf, CB_SIZE));
		CHECK_EQ(read_exact(ni, buf, CB_SIZE, 4194304 - CB_SIZE), 0, "read the last hole");
		CHECK(all_zero(buf, CB_SIZE));
		/* No data anywhere, so SEEK_DATA has nothing to report. */
		CHECK_EQ(ntfs_seek_data_hole(ni, 0, false, &r), -ENXIO, "SEEK_DATA");
		CHECK_EQ(ntfs_seek_data_hole(ni, 0, true, &r), 0, "SEEK_HOLE");
		CHECK_EQ(r, 0, "the hole starts at 0");
		ntfs_inode_put(ni);
	}

	/* (b) a hole with the data at the very end: the allocation must be far
	 * smaller than the file, and the boundary must be where the run list
	 * says it is. */
	ni = open_file("sparse", "tail-data.bin");
	CHECK(ni != NULL);
	if (ni) {
		struct ntfs_attr a;
		CHECK(ntfs_getattr(ni, &a) == 0);
		CHECK(a.sparse);
		CHECK(a.alloc_size < a.size / 100);	/* one cluster of 3 MiB */
		CHECK_EQ(read_exact(ni, buf, CB_SIZE, 0), 0, "read the leading hole");
		CHECK(all_zero(buf, CB_SIZE));
		CHECK_EQ(ntfs_seek_data_hole(ni, 0, true, &r), 0, "SEEK_HOLE at 0");
		CHECK_EQ(r, 0, "the file opens with a hole");
		CHECK_EQ(ntfs_seek_data_hole(ni, 0, false, &r), 0, "SEEK_DATA");
		CHECK(r > 0 && r < a.size);
		CHECK_EQ(r % 4096, 0, "the data boundary is cluster aligned");
		/* The tail really is data. */
		CHECK_EQ(read_exact(ni, buf, 4096, r), 0, "read the tail");
		CHECK(!all_zero(buf, 4096));
		ntfs_inode_put(ni);
	}

	/* (c) alternating holes and data. */
	ni = open_file("sparse", "holes-8m.bin");
	CHECK(ni != NULL);
	if (ni) {
		struct ntfs_attr a;
		uint64_t pos = 0, data_at, hole_at;
		int transitions = 0;

		CHECK(ntfs_getattr(ni, &a) == 0);
		CHECK(a.sparse);
		CHECK(a.alloc_size < a.size);
		/* Walk the file by alternating SEEK_DATA / SEEK_HOLE. Every hole
		 * reported must read back as zeroes; a run list the seek code and
		 * the read code disagree about shows up here and nowhere else. */
		while (pos < a.size && transitions < 64) {
			if (ntfs_seek_data_hole(ni, pos, false, &data_at))
				break;
			if (ntfs_seek_data_hole(ni, data_at, true, &hole_at))
				break;
			if (hole_at >= a.size)
				break;
			transitions++;
			{
				size_t n = (size_t)((a.size - hole_at) < 4096 ?
						    (a.size - hole_at) : 4096);
				CHECK_EQ(read_exact(ni, buf, n, hole_at), 0, "read a hole");
				CHECK(all_zero(buf, n));
			}
			pos = hole_at + 4096;
		}
		CHECK(transitions > 0);		/* else the fixture has no holes */
		ntfs_inode_put(ni);
	}

	/* (d) a 1 GiB file that costs a couple of clusters. Guards against the
	 * read path materialising a hole instead of synthesising it. */
	ni = open_file("sparse", "huge-1g.bin");
	CHECK(ni != NULL);
	if (ni) {
		struct ntfs_attr a;
		uint64_t hole;

		CHECK(ntfs_getattr(ni, &a) == 0);
		CHECK_EQ(a.size, 1073741824, "huge size");
		CHECK(a.alloc_size <= 65536);	/* a gigabyte for a couple of clusters */
		/* Read inside a hole the run list actually reports, rather than
		 * at a guessed offset: where the fixture put its data is the
		 * fixture's business. */
		CHECK_EQ(ntfs_seek_data_hole(ni, 0, true, &hole), 0, "SEEK_HOLE");
		CHECK(hole < a.size - CB_SIZE);
		CHECK_EQ(read_exact(ni, buf, CB_SIZE, hole), 0, "read mid-file hole");
		CHECK(all_zero(buf, CB_SIZE));
		ntfs_inode_put(ni);
	}

	free(buf);
	umount_scratch();
	unlink(scratch);
}

/*
 * The write side of sparseness. ntfs_fallocate() is the only allocation entry
 * point in the ABI and it exposes preallocation only: core/vfs/api.c passes
 * FALLOC_FL_KEEP_SIZE or nothing, so ntfs_vfs_fallocate()'s FALLOC_FL_PUNCH_HOLE
 * branch -- and with it ntfs_punch_hole() -- cannot be reached from outside the
 * core at all. Holes are therefore only ever made the other way: by leaving a
 * gap, which is what the second half of this test does.
 */
static void test_sparse_write(void)
{
	unsigned char *buf;
	ntfs_inode_t *root, *dir, *f = NULL;
	uint64_t r;

	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_sparse_write: no fixture\n");
		return;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	buf = malloc(CB_SIZE);
	CHECK(buf != NULL);
	CHECK_EQ(ntfs_volume_root(g_vol, &root), 0, "root");
	CHECK_EQ(ntfs_lookup(root, "sparse", &dir), 0, "lookup /sparse");
	CHECK_EQ(ntfs_create(dir, "falloc.bin", 0100644, &f), 0, "create");
	if (!buf || !f) goto out;

	/* (a) preallocation with keep_size must move the allocation and not the
	 * length. A fallocate that grows i_size is the classic way to expose
	 * uninitialised disk contents to the caller. */
	CHECK_EQ(ntfs_fallocate(f, 0, 1u << 20, true), 0, "fallocate keep_size");
	CHECK_EQ(data_size(f), 0, "keep_size leaves i_size alone");
	CHECK(on_disk_size(f) >= (1u << 20));

	/* (b) the same range without keep_size publishes it, and it must read
	 * as zeroes rather than as whatever those clusters held before. */
	CHECK_EQ(ntfs_fallocate(f, 0, 1u << 20, false), 0, "fallocate");
	CHECK_EQ(data_size(f), 1u << 20, "i_size follows the allocation");
	CHECK_EQ(read_exact(f, buf, CB_SIZE, 0), 0, "read preallocated");
	CHECK(all_zero(buf, CB_SIZE));
	/* Preallocated but never written is still data, not a hole. */
	CHECK_EQ(ntfs_seek_data_hole(f, 0, false, &r), 0, "SEEK_DATA in preallocation");
	CHECK_EQ(r, 0, "preallocated clusters are data");

	/* (c) shrink, then write far past the end. The gap must become a hole:
	 * the allocation stays small, the inode becomes sparse, and reading the
	 * gap yields zeroes rather than the clusters freed in the shrink. */
	CHECK_EQ(ntfs_truncate(f, 4096), 0, "truncate down");
	memset(buf, 'D', 4096);
	CHECK_EQ(ntfs_write(f, buf, 4096, 4u << 20), 4096, "write past the gap");
	CHECK_EQ(data_size(f), (4u << 20) + 4096, "size after the sparse extend");
	{
		struct ntfs_attr a;
		CHECK(ntfs_getattr(f, &a) == 0);
		CHECK(a.sparse);
		CHECK(a.alloc_size < 65536);	/* two clusters, not four megabytes */
	}
	CHECK_EQ(read_exact(f, buf, CB_SIZE, 1u << 20), 0, "read the gap");
	CHECK(all_zero(buf, CB_SIZE));
	CHECK_EQ(ntfs_seek_data_hole(f, 8192, true, &r), 0, "SEEK_HOLE");
	CHECK_EQ(r, 8192, "the hole starts right after the first cluster");
	CHECK_EQ(ntfs_seek_data_hole(f, 8192, false, &r), 0, "SEEK_DATA");
	CHECK_EQ(r, 4u << 20, "the data resumes where it was written");

	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "volume sync");
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(dir);
	ntfs_inode_put(root);
	umount_scratch();
	fsck("sparse_write");

	/* The hole and the data must still be where they were after a remount:
	 * an in-memory run list that the mapping pairs do not describe reads
	 * perfectly until the volume comes back. */
	CHECK_EQ(mount_scratch(), 0, "remount");
	f = open_file("sparse", "falloc.bin");
	CHECK(f != NULL);
	if (f) {
		CHECK_EQ(data_size(f), (4u << 20) + 4096, "size after remount");
		CHECK_EQ(read_exact(f, buf, CB_SIZE, 1u << 20), 0, "read the gap");
		CHECK(all_zero(buf, CB_SIZE));
		CHECK_EQ(read_exact(f, buf, 4096, 4u << 20), 0, "read the tail");
		for (int i = 0; i < 4096; i++)
			if (buf[i] != 'D') { CHECK(!"tail data survived"); break; }
		ntfs_inode_put(f);
		f = NULL;
	}
	free(buf);
	umount_scratch();
	unlink(scratch);
	return;
out:
	if (f) ntfs_inode_put(f);
	free(buf);
	umount_scratch();
	unlink(scratch);
}

/* ---- 8. encrypted inodes are refused in both directions --------------- */

/*
 * No fixture carries an encrypted file -- mkntfs cannot make one, EFS needs
 * Windows -- so this builds one: create an ordinary file, then set
 * ATTR_IS_ENCRYPTED on its $DATA attribute record by hand. That single bit is
 * what core/vfs/inode.c turns into NInoEncrypted, and NInoEncrypted is the
 * whole of the port's EFS support: refuse. The refusal is what protects a
 * user's encrypted documents from being overwritten with plaintext, so it is
 * worth a test even though it implements nothing.
 */
#define MFT_ATTRS_OFFSET	0x14
#define MFT_BYTES_IN_USE	0x18
#define ATTR_TYPE		0x00
#define ATTR_LENGTH		0x04
#define ATTR_FLAGS		0x0c
#define ATTR_VALUE_OFFSET	0x14	/* resident attributes only */
#define AT_STANDARD_INFO_TYPE	0x10u
#define AT_DATA_TYPE		0x80u
#define AT_END_TYPE		0xffffffffu
#define SI_FILE_ATTRIBUTES	0x20	/* within the $STANDARD_INFORMATION value */
#define ENCRYPTED_BIT		0x4000u	/* ATTR_IS_ENCRYPTED and FILE_ATTR_ENCRYPTED
					 * happen to be the same bit */

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

/*
 * Mark MFT record @mft_no encrypted: the ATTR_IS_ENCRYPTED flag on its unnamed
 * $DATA, which is what the driver reads, and FILE_ATTR_ENCRYPTED in
 * $STANDARD_INFORMATION, which is what ntfsck cross-checks it against. Setting
 * only the first leaves an image ntfsck calls corrupt, and a test whose subject
 * is a broken file proves nothing about encrypted files.
 */
static int mark_encrypted(uint64_t mft_no)
{
	struct ntfs_image img;
	uint8_t *rec;
	uint64_t off;
	uint32_t pos, in_use;
	int fd, err, found = 0;
	bool torn;

	err = ntfs_image_open(&img, scratch, true, 0, 0);
	if (err)
		return err;
	rec = malloc(img.mft_record_size);
	if (!rec) { ntfs_image_close(&img); return -ENOMEM; }

	off = ntfs_image_map(img.mft_runs, img.n_mft_runs, img.cluster_size,
			     mft_no * img.mft_record_size);
	if (off == UINT64_MAX) { err = -EIO; goto out; }

	fd = open(scratch, O_RDWR);
	if (fd < 0) { err = -errno; goto out; }
	if (pread(fd, rec, img.mft_record_size, (off_t)off) != (ssize_t)img.mft_record_size) {
		err = -EIO; close(fd); goto out;
	}
	if (memcmp(rec, "FILE", 4) != 0) { err = -EINVAL; close(fd); goto out; }
	err = ntfs_log_fixup_post_read(rec, img.mft_record_size, 512, &torn);
	if (err || torn) { err = err ? err : -EIO; close(fd); goto out; }

	in_use = get32(rec + MFT_BYTES_IN_USE);
	for (pos = get16(rec + MFT_ATTRS_OFFSET); pos + 8 <= in_use; ) {
		uint32_t type = get32(rec + pos + ATTR_TYPE);
		uint32_t len = get32(rec + pos + ATTR_LENGTH);

		if (type == AT_END_TYPE || !len || pos + len > in_use)
			break;
		if (type == AT_STANDARD_INFO_TYPE) {
			uint8_t *v = rec + pos + get16(rec + pos + ATTR_VALUE_OFFSET);

			put32(v + SI_FILE_ATTRIBUTES,
			      get32(v + SI_FILE_ATTRIBUTES) | ENCRYPTED_BIT);
		}
		if (type == AT_DATA_TYPE) {
			put16(rec + pos + ATTR_FLAGS,
			      (uint16_t)(get16(rec + pos + ATTR_FLAGS) | ENCRYPTED_BIT));
			found = 1;
			break;
		}
		pos += len;
	}
	if (!found) { err = -ENOENT; close(fd); goto out; }

	err = ntfs_log_fixup_pre_write(rec, img.mft_record_size, 512);
	if (!err && pwrite(fd, rec, img.mft_record_size, (off_t)off) !=
		    (ssize_t)img.mft_record_size)
		err = -EIO;
	close(fd);
out:
	free(rec);
	ntfs_image_close(&img);
	return err;
}

static void test_encrypted_refused(void)
{
	unsigned char buf[4096];
	ntfs_inode_t *root, *dir, *f;
	uint64_t mft_no, r;

	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_encrypted_refused: no fixture\n");
		return;
	}
	/* /compressed is not itself compressed, so a file made in it is plain:
	 * inode.c refuses an inode that claims to be encrypted AND compressed,
	 * which would be a different test. */
	if (mount_scratch()) { failures++; checks++; unlink(scratch); return; }
	CHECK_EQ(ntfs_volume_root(g_vol, &root), 0, "root");
	CHECK_EQ(ntfs_lookup(root, "compressed", &dir), 0, "lookup /compressed");
	CHECK_EQ(ntfs_create(dir, "efs.bin", 0100644, &f), 0, "create");
	if (!f) { ntfs_inode_put(dir); ntfs_inode_put(root);
		  umount_scratch(); unlink(scratch); return; }
	mft_no = ntfs_inode_number(f);
	memset(buf, 'p', sizeof(buf));
	/* Non-resident, so the encrypted inode has a real run list to refuse. */
	CHECK_EQ(ntfs_write(f, buf, sizeof(buf), 0), sizeof(buf), "seed write");
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "volume sync");
	ntfs_inode_put(f);
	ntfs_inode_put(dir);
	ntfs_inode_put(root);
	umount_scratch();

	CHECK_EQ(mark_encrypted(mft_no), 0, "set ATTR_IS_ENCRYPTED");
	fsck("encrypted flag");

	CHECK_EQ(mount_scratch(), 0, "remount");
	f = open_file("compressed", "efs.bin");
	CHECK(f != NULL);
	if (f) {
		struct ntfs_attr a;

		CHECK(ntfs_getattr(f, &a) == 0);
		CHECK(a.encrypted);		/* else the patch did not take */
		CHECK(!a.compressed);
		CHECK_EQ(a.size, 4096, "size still readable from the metadata");

		/* Reading would hand the caller ciphertext as if it were the
		 * file; writing would destroy the ciphertext. Both refuse. */
		CHECK_EQ(ntfs_read(f, buf, sizeof(buf), 0), -EOPNOTSUPP, "read refused");
		CHECK_EQ(ntfs_write(f, buf, sizeof(buf), 0), -EOPNOTSUPP, "write refused");
		CHECK_EQ(ntfs_truncate(f, 0), -EOPNOTSUPP, "truncate refused");
		CHECK_EQ(ntfs_truncate(f, 8192), -EOPNOTSUPP, "extend refused");
		CHECK_EQ(ntfs_fallocate(f, 0, 65536, true), -EOPNOTSUPP,
			 "fallocate keep_size refused");
		CHECK_EQ(ntfs_fallocate(f, 0, 65536, false), -EOPNOTSUPP,
			 "fallocate refused");
		CHECK_EQ(data_size(f), 4096, "nothing changed under the refusals");
		/* Metadata is still usable: only the data is off limits. */
		CHECK_EQ(ntfs_seek_data_hole(f, 0, false, &r), 0, "SEEK_DATA still works");
		ntfs_inode_put(f);
	}
	umount_scratch();
	fsck("encrypted_refused");
	unlink(scratch);
}

/* ---- 9. a compression block that cannot have contiguous clusters ------ */

/*
 * Finding 30 (= U18) in docs/UPSTREAM-BUGS.md. ntfs_write_cb() asks the
 * allocator for the block's clusters as one contiguous run and wrote the whole
 * block from the first of them. Where the free space is in pieces smaller than
 * the block, the allocator returns the longest piece it finds, short, and the
 * rest of the block went onto the clusters after it, which belong to another
 * file. Measured before the fix: the write returned 65536, the block then read
 * back -EILSEQ (it had 8 of its 16 clusters, so it read as compressed), the
 * file next to the free piece had its 8 clusters replaced by the block's
 * second half, and ntfsck -n was clean.
 *
 * The free space is made of 8-cluster gaps between 8-cluster files: the small
 * files first, the rest of the volume filled, then every other small file
 * deleted. A block of random bytes does not compress, so it is stored raw and
 * needs 16 clusters. The free count is checked before the write: anything free
 * beyond the gaps should be fewer clusters than a gap. And after it, the
 * block's clusters are found on the image and must lie in two runs at least:
 * a block that got 16 contiguous clusters tested nothing.
 */
#define FRAG_FILES	64
#define FRAG_CLUSTERS	8
#define FRAG_CS		4096u	/* the fixture's cluster size, checked below */

/* The cluster number on the scratch image of each 4 KiB piece of @blk, or -1. */
static void frag_find_clusters(const unsigned char *blk, int n, long long *lcn)
{
	unsigned char c[FRAG_CS];
	off_t off;
	int fd, k;

	for (k = 0; k < n; k++)
		lcn[k] = -1;
	fd = open(scratch, O_RDONLY);
	if (fd < 0)
		return;
	for (off = 0; pread(fd, c, FRAG_CS, off) == (ssize_t)FRAG_CS; off += FRAG_CS)
		for (k = 0; k < n; k++)
			if (lcn[k] < 0 && !memcmp(c, blk + (size_t)k * FRAG_CS, FRAG_CS)) {
				lcn[k] = (long long)(off / FRAG_CS);
				break;
			}
	close(fd);
}

static void frag_pattern(unsigned char *p, size_t n, int k)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (unsigned char)('A' + (k * 7 + (int)(i >> 12)) % 26);
}

/*
 * Fill the rest of the volume with files in @dir, a file at a time: one file's
 * extension can be refused while clusters are free elsewhere, a fresh file's
 * first cluster cannot. @buf is 1 MiB of scratch.
 */
static void fill_volume(ntfs_inode_t *dir, unsigned char *buf)
{
	const size_t mib = 1u << 20;
	ntfs_inode_t *f;
	char name[32];
	int i;

	memset(buf, 'Z', mib);
	for (i = 0; i < 16; i++) {
		size_t chunk = mib;
		uint64_t off = 0;

		snprintf(name, sizeof(name), "fill%02d", i);
		if (ntfs_create(dir, name, 0100644, &f))
			break;
		for (;;) {
			ssize_t w = ntfs_write(f, buf, chunk, off);

			if (w == (ssize_t)chunk) { off += chunk; continue; }
			if (chunk == 4096) break;
			chunk = 4096;
		}
		ntfs_inode_put(f);
		if (!off)
			break;
	}
}

static void test_fragmented_rewrite(void)
{
	const size_t small = FRAG_CLUSTERS * FRAG_CS, mib = 1u << 20;
	unsigned char *buf = malloc(mib), *rb = malloc(mib), *orig = malloc(mib);
	unsigned char *blk = malloc(CB_SIZE);
	ntfs_inode_t *root = NULL, *dir = NULL, *f;
	struct ntfs_volume_info info;
	char name[32];
	int i, damaged = 0;

	if (!buf || !rb || !orig || !blk) {
		CHECK(!"out of memory");
		goto free;
	}
	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_fragmented_rewrite: no fixture\n");
		goto free;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); goto free; }
	ntfs_volume_get_info(g_vol, &info);
	CHECK_EQ(info.cluster_size, FRAG_CS, "the fixture's cluster size");
	if (info.cluster_size != FRAG_CS) goto out;

	f = open_file("compressed", "text-1m.txt");
	CHECK(f != NULL);
	if (!f) goto out;
	CHECK_EQ(read_exact(f, orig, mib, 0), 0, "read text-1m.txt");
	ntfs_inode_put(f);

	CHECK_EQ(ntfs_volume_root(g_vol, &root), 0, "root");
	CHECK_EQ(ntfs_mkdir(root, "frag", 0755, &dir), 0, "mkdir /frag");
	if (!root || !dir) goto out;
	for (i = 0; i < FRAG_FILES; i++) {
		snprintf(name, sizeof(name), "s%02d", i);
		if (ntfs_create(dir, name, 0100644, &f)) { CHECK(!"create a small file"); break; }
		frag_pattern(buf, small, i);
		CHECK_EQ(ntfs_write(f, buf, small, 0), small, "write a small file");
		ntfs_inode_put(f);
	}
	fill_volume(dir, buf);
	ntfs_inode_put(dir);
	dir = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "sync after the fill");
	umount_scratch();

	CHECK_EQ(mount_scratch(), 0, "remount");
	if (!g_vol) goto out;
	CHECK_EQ(ntfs_volume_root(g_vol, &root), 0, "root");
	CHECK_EQ(ntfs_lookup(root, "frag", &dir), 0, "lookup /frag");
	if (!dir) goto out;
	for (i = 0; i < FRAG_FILES; i += 2) {
		snprintf(name, sizeof(name), "s%02d", i);
		CHECK_EQ(ntfs_unlink(dir, name), 0, "unlink every other small file");
	}
	ntfs_inode_put(dir);
	dir = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "sync after the unlinks");
	ntfs_volume_get_info(g_vol, &info);
	checks++;
	if (info.free_clusters < (FRAG_FILES / 2) * FRAG_CLUSTERS ||
	    info.free_clusters >= (FRAG_FILES / 2 + 1) * FRAG_CLUSTERS) {
		failures++;
		fprintf(stderr, "FAIL fragmented_rewrite: %llu clusters free after the "
			"unlinks, expected %d gaps of %d and less than one gap more: "
			"16 contiguous clusters may be free, and the case untested\n",
			(unsigned long long)info.free_clusters, FRAG_FILES / 2,
			FRAG_CLUSTERS);
	}
	umount_scratch();

	/* The rewrite, on a fresh mount: random bytes over block 1. */
	CHECK_EQ(mount_scratch(), 0, "remount");
	f = g_vol ? open_file("compressed", "text-1m.txt") : NULL;
	CHECK(f != NULL);
	if (!f) goto out;
	fill_random(blk, CB_SIZE, 30);
	CHECK_EQ(ntfs_write(f, blk, CB_SIZE, CB_SIZE), CB_SIZE, "rewrite block 1 raw");
	ntfs_inode_put(f);
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "sync after the rewrite");
	umount_scratch();
	fsck("fragmented_rewrite");
	{
		long long lcn[CB_SIZE / FRAG_CS];
		int n = (int)(CB_SIZE / FRAG_CS), found = 0, runs = 1;

		frag_find_clusters(blk, n, lcn);
		for (i = 0; i < n; i++)
			found += lcn[i] >= 0;
		for (i = 1; i < n; i++)
			runs += lcn[i] != lcn[i - 1] + 1;
		CHECK_EQ(found, n, "clusters of the rewritten block found on the image");
		checks++;
		if (found == n && runs < 2) {
			failures++;
			fprintf(stderr, "FAIL fragmented_rewrite: the block's %d clusters are "
				"contiguous on the disk (from LCN %lld): either it was given 16 "
				"free clusters in a row and this tested nothing, or it was "
				"written past the run it was given\n", n, lcn[0]);
		}
	}

	/* After a remount: the block, the rest of the file, and the neighbours. */
	CHECK_EQ(mount_scratch(), 0, "remount");
	f = g_vol ? open_file("compressed", "text-1m.txt") : NULL;
	CHECK(f != NULL);
	if (f) {
		memcpy(orig + CB_SIZE, blk, CB_SIZE);
		CHECK_EQ(read_exact(f, rb, mib, 0), 0, "read text-1m.txt back");
		compare("text-1m.txt after a fragmented rewrite", rb, orig, mib);
		ntfs_inode_put(f);
	}
	for (i = 1; g_vol && i < FRAG_FILES; i += 2) {
		snprintf(name, sizeof(name), "s%02d", i);
		f = open_file("frag", name);
		frag_pattern(buf, small, i);
		if (!f || read_exact(f, rb, small, 0) || memcmp(rb, buf, small))
			damaged++;
		if (f)
			ntfs_inode_put(f);
	}
	CHECK_EQ(damaged, 0, "small files damaged by the rewrite");
out:
	if (dir) ntfs_inode_put(dir);
	if (root) ntfs_inode_put(root);
	umount_scratch();
	unlink(scratch);
free:
	free(buf);
	free(rb);
	free(orig);
	free(blk);
}

/* ---- 10. a rewrite that fails must leave the old block ---------------- */

/*
 * Finding 32 (= U21) in docs/UPSTREAM-BUGS.md. ntfs_write_cb() punched the
 * block's old clusters out before it allocated the new ones, and gave nothing
 * back when the allocation or the write failed. Measured before the fix, on
 * this fixture filled to its last cluster: random bytes over block 1 of
 * text-1m.txt, which holds 2 clusters of compressed text, are stored raw and
 * need 16 clusters. The punch freed 2, the allocation failed, the write
 * returned -ENOSPC, and after a remount the block read back as 65,536 zeroes.
 * ntfsck -n was clean.
 *
 * Neither rewrite below can succeed: one has no room, and in the other the
 * device refuses the block's write. Each must fail and leave the old block as
 * it was, in the free count, on the same mount, and after a remount. The same
 * mount matters because the block's pages took the new bytes before the write
 * was tried. A failed write has to drop them, or a read returns bytes that
 * never reached the disk, and the next write to the block puts them there.
 */
static const char *label(const char *tag, const char *what)
{
	static char buf[192];

	snprintf(buf, sizeof(buf), "%s: %s", tag, what);
	return buf;
}

/* A rewrite of block 1 that must fail and leave the block as it was. */
struct refused_rewrite {
	const char *tag;
	const char *file;		/* a 1 MiB file in /compressed */
	const unsigned char *blk;	/* the new block 1 */
	ssize_t want;			/* what the rewrite returns */
	bool poke;			/* a small write into the block follows */
	bool read_only;			/* the failure takes the volume read-only */
	uint64_t refuse_lo, refuse_hi;	/* device reads refused during the rewrite */
};

/*
 * Run @r on the mounted scratch volume and check that the block is as it was:
 * in the free count, on the same mount, and after a remount, with ntfsck -n
 * in between. With r->poke, a small write into the block follows on the same
 * mount, and @orig is updated to match. @orig is the whole file before the
 * writes, and @rb is 1 MiB of scratch. Leaves the volume mounted again after
 * the remount: read-only, when r->read_only and the unmount left the volume
 * marked dirty.
 */
static void rewrite_refused(const struct refused_rewrite *r, unsigned char *orig,
			    unsigned char *rb)
{
	const size_t mib = 1u << 20;
	const char *tag = r->tag;
	struct ntfs_volume_info info;
	ntfs_inode_t *f;
	uint64_t free0, csize0;
	ssize_t w;

	f = g_vol ? open_file("compressed", r->file) : NULL;
	CHECK(f != NULL);
	if (!f)
		return;
	ntfs_volume_get_info(g_vol, &info);
	free0 = info.free_clusters;
	/* A failed mapping-pairs update may have rewritten this already. */
	csize0 = on_disk_size(f);
	refused_reads = 0;
	refuse_read_lo = r->refuse_lo;
	refuse_read_hi = r->refuse_hi;
	w = ntfs_write(f, r->blk, CB_SIZE, CB_SIZE);
	refuse_read_lo = refuse_read_hi = 0;
	CHECK_EQ(w, r->want, label(tag, "rewrite block 1"));
	ntfs_volume_get_info(g_vol, &info);
	CHECK_EQ(info.free_clusters, free0, label(tag, "free clusters after the failed rewrite"));
	CHECK_EQ(info.read_only, r->read_only, label(tag, "read-only after the failed rewrite"));
	CHECK_EQ(on_disk_size(f), csize0, label(tag, "compressed size after the failed rewrite"));
	if (r->poke) {
		/* It takes the rest of the block from the block's pages, so
		 * they must hold the old bytes again: if they still held the
		 * refused ones, this write would put them on the disk. */
		static const char text[] = "a small write into block 1 after the failed rewrite";
		const size_t n = sizeof(text) - 1;

		memcpy(orig + CB_SIZE + 100, text, n);
		CHECK_EQ(ntfs_write(f, text, n, CB_SIZE + 100), n,
			 label(tag, "a small write into block 1"));
		ntfs_volume_get_info(g_vol, &info);
		free0 = info.free_clusters;
	}
	CHECK_EQ(read_exact(f, rb, CB_SIZE, CB_SIZE), 0, label(tag, "read block 1 again"));
	compare(label(tag, "block 1 on the mount the rewrite failed on"), rb,
		orig + CB_SIZE, CB_SIZE);
	ntfs_inode_put(f);
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, label(tag, "sync after the failed rewrite"));
	umount_scratch();
	fsck(tag);

	/* Only reads follow, so a read-only remount does as well. */
	CHECK_EQ(mount_scratch_after(r->read_only), 0, label(tag, "remount"));
	f = g_vol ? open_file("compressed", r->file) : NULL;
	CHECK(f != NULL);
	if (!f)
		return;
	CHECK_EQ(read_exact(f, rb, mib, 0), 0, label(tag, "read the file after a remount"));
	compare(label(tag, "the file after a remount"), rb, orig, mib);
	if (!r->poke)
		CHECK_EQ(on_disk_size(f), csize0, label(tag, "compressed size after a remount"));
	ntfs_inode_put(f);
	ntfs_volume_get_info(g_vol, &info);
	CHECK_EQ(info.free_clusters, free0, label(tag, "free clusters after a remount"));
}

static void test_full_volume_rewrite(void)
{
	const size_t mib = 1u << 20;
	unsigned char *buf = malloc(mib), *rb = malloc(mib), *orig = malloc(mib);
	unsigned char *blk = malloc(CB_SIZE);
	ntfs_inode_t *root = NULL, *dir = NULL, *f;
	struct ntfs_volume_info info;
	uint64_t free0;

	if (!buf || !rb || !orig || !blk) {
		CHECK(!"out of memory");
		goto free;
	}
	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_full_volume_rewrite: no fixture\n");
		goto free;
	}
	if (mount_scratch()) { failures++; checks++; unlink(scratch); goto free; }
	ntfs_volume_get_info(g_vol, &info);
	CHECK_EQ(info.cluster_size, FRAG_CS, "the fixture's cluster size");
	if (info.cluster_size != FRAG_CS) goto out;

	f = open_file("compressed", "text-1m.txt");
	CHECK(f != NULL);
	if (!f) goto out;
	CHECK_EQ(read_exact(f, orig, mib, 0), 0, "read text-1m.txt");
	ntfs_inode_put(f);

	CHECK_EQ(ntfs_volume_root(g_vol, &root), 0, "root");
	CHECK_EQ(ntfs_mkdir(root, "full", 0755, &dir), 0, "mkdir /full");
	if (!root || !dir) goto out;
	fill_volume(dir, buf);
	ntfs_inode_put(dir);
	dir = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "sync after the fill");
	umount_scratch();

	/* On a fresh mount, so the block is read from the disk. */
	CHECK_EQ(mount_scratch(), 0, "remount");
	if (!g_vol) goto out;
	ntfs_volume_get_info(g_vol, &info);
	free0 = info.free_clusters;
	/* The small rewrite below needs this: with a cluster free, it would fit
	 * beside the old block and succeed. */
	CHECK_EQ(free0, 0, "free clusters after the fill");
	fill_random(blk, CB_SIZE, 32);
	rewrite_refused(&(struct refused_rewrite){ .tag = "full_volume_rewrite",
			.file = "text-1m.txt", .blk = blk, .want = -ENOSPC }, orig, rb);

	/*
	 * One repeated byte compresses to a single cluster, which the block's
	 * own 2 could hold. It is refused as well, because the new block needs
	 * room while the old one still holds its clusters. The old order made
	 * that room by punching first. This pins the trade-off that keeps the
	 * old block safe instead.
	 */
	memset(blk, 'x', CB_SIZE);
	rewrite_refused(&(struct refused_rewrite){ .tag = "full_volume_small_rewrite",
			.file = "text-1m.txt", .blk = blk, .want = -ENOSPC }, orig, rb);

	/*
	 * What the old order had to work with: the free clusters, and the
	 * block's own, which its punch released first. Zeroing the block
	 * releases them now and counts them. Unless the block held some and
	 * the two come to fewer than the 16 a raw block needs, the old order
	 * lost nothing here and this case tested nothing.
	 */
	f = g_vol ? open_file("compressed", "text-1m.txt") : NULL;
	CHECK(f != NULL);
	if (!f) goto out;
	memset(blk, 0, CB_SIZE);
	CHECK_EQ(ntfs_write(f, blk, CB_SIZE, CB_SIZE), CB_SIZE, "zero block 1 to count its clusters");
	ntfs_inode_put(f);
	ntfs_volume_get_info(g_vol, &info);
	checks++;
	if (info.free_clusters <= free0 || info.free_clusters >= CB_SIZE / FRAG_CS) {
		failures++;
		fprintf(stderr, "FAIL full_volume_rewrite: %llu clusters were free and block 1 "
			"held %lld: the old order fails only when the block held some "
			"and the two come to fewer than %u\n",
			(unsigned long long)free0,
			(long long)info.free_clusters - (long long)free0, CB_SIZE / FRAG_CS);
	}
out:
	if (dir) ntfs_inode_put(dir);
	if (root) ntfs_inode_put(root);
	umount_scratch();
	unlink(scratch);
free:
	free(buf);
	free(rb);
	free(orig);
	free(blk);
}

/*
 * The same rewrite with room to spare, on a device that refuses the block's
 * write. The new clusters were allocated by then, so this is the case that
 * has to give them back. With room, a small write into the block can follow
 * on the same mount, and it must not bring the refused bytes back.
 */
static void test_rewrite_write_error(void)
{
	const size_t mib = 1u << 20;
	unsigned char *rb = malloc(mib), *orig = malloc(mib), *blk = malloc(CB_SIZE);
	ntfs_inode_t *f;

	if (!rb || !orig || !blk) {
		CHECK(!"out of memory");
		goto free;
	}
	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP test_rewrite_write_error: no fixture\n");
		goto free;
	}
	g_refusing = true;
	if (mount_scratch()) { failures++; checks++; goto out; }
	f = open_file("compressed", "text-1m.txt");
	CHECK(f != NULL);
	if (!f) goto out;
	CHECK_EQ(read_exact(f, orig, mib, 0), 0, "read text-1m.txt");
	ntfs_inode_put(f);

	fill_random(blk, CB_SIZE, 33);
	refuse_data = blk;
	refused = 0;
	rewrite_refused(&(struct refused_rewrite){ .tag = "rewrite_write_error",
			.file = "text-1m.txt", .blk = blk, .want = -EIO, .poke = true },
			orig, rb);
	/* A device that never refused anything tested nothing. */
	CHECK_EQ(refused, 1, "writes of the new block refused");
out:
	g_refusing = false;
	refuse_data = NULL;
	umount_scratch();
	unlink(scratch);
free:
	free(rb);
	free(orig);
	free(blk);
}

/* ---- 11. a rewrite whose mapping pairs cannot be written --------------- */

/*
 * U22 in docs/UPSTREAM-BUGS.md, finding 32's last open path. Finding 32's fix
 * wrote the new block first, then punched the old block out and merged the
 * new runs in, two mapping-pairs updates. A failure after the punch lost the
 * block. The punch's own update fails when taking the block out of the middle
 * of a run splits the run and the record has no room for the extra pairs. The
 * second update fails when the new block lies in more runs than the old one
 * and its pairs no longer fit. Either way the run list was left with a hole
 * there: the block read back as zeroes on the same mount, and the next write
 * of the file's mapping pairs could put the hole on the disk. A block of
 * zeroes over one that holds data is punched alone (U7), and its punch had
 * the same single update.
 *
 * The update fails here as it does when an MFT record cannot be had. The
 * file's base record is filled with the largest resident xattr, so more pairs
 * need an attribute list, which needs an extent record. Every read of $MFT is
 * refused during the rewrite, on a fresh mount, so that record cannot be
 * mapped. That fails inside ntfs_inode_add_attrlist(), before any mapping
 * pairs are written, and ntfs_error() takes the volume read-only. So these
 * cases check that the run list and the sizes are put back; putting back
 * pairs that a failed update had already written is reached only by hooks.
 *
 * split_run_update_fails: random-1m.bin is one run of 256 clusters, so taking
 * block 1 out of it splits the run. zero_block_update_fails: the same with
 * zeroes, which the U7 path punches. fragmented_update_fails: block 1 of
 * text-1m.txt is 2 clusters and a hole, and with the free space in 2-cluster
 * gaps the new raw block lies in 8 runs. In each, the rewrite must fail with
 * -EIO, the block must read back as it was on the same mount and after a
 * remount, the free count and the compressed size must not move, and ntfsck
 * -n must be clean. A case that refused no read of $MFT tested nothing.
 */
#define FILL_XATTR	"fill"
#define GAP_FILES	24
#define GAP_CLUSTERS	2

/* $MFT on the scratch image, from the boot sector and the record count, with
 * room for the records the allocator may add: [*lo, *hi). */
static int mft_range(uint64_t *lo, uint64_t *hi)
{
	struct ntfs_volume_info info;
	uint8_t bs[512];
	uint64_t lcn = 0;
	ssize_t n;
	int fd, i;

	if (!g_vol || ntfs_volume_get_info(g_vol, &info))
		return -EINVAL;
	fd = open(scratch, O_RDONLY);
	if (fd < 0)
		return -errno;
	n = pread(fd, bs, sizeof(bs), 0);
	close(fd);
	if (n != (ssize_t)sizeof(bs))
		return -EIO;
	for (i = 7; i >= 0; i--)
		lcn = lcn << 8 | bs[0x30 + i];
	*lo = lcn * info.cluster_size;
	*hi = *lo + (info.total_mft_records + 64) * (uint64_t)info.mft_record_size;
	return 0;
}

/*
 * Fill @f's base MFT record with the largest resident xattr, found by halving:
 * a size is resident if it costs no cluster. Returns the size, or 0. @buf is
 * 4 KiB of scratch.
 */
static uint64_t fill_base_record(ntfs_inode_t *f, unsigned char *buf)
{
	struct ntfs_volume_info info;
	uint64_t lo = 0, hi, before;

	if (ntfs_volume_get_info(g_vol, &info))
		return 0;
	memset(buf, 'x', 4096);
	for (hi = info.mft_record_size; lo + 1 < hi; ) {
		uint64_t mid = (lo + hi) / 2;
		int err;

		ntfs_volume_get_info(g_vol, &info);
		before = info.free_clusters;
		err = ntfs_setxattr(f, FILL_XATTR, buf, mid, 0);
		ntfs_volume_get_info(g_vol, &info);
		if (!err && info.free_clusters == before)
			lo = mid;
		else
			hi = mid;
		ntfs_removexattr(f, FILL_XATTR);
	}
	if (!lo)
		return 0;
	ntfs_volume_get_info(g_vol, &info);
	before = info.free_clusters;
	if (ntfs_setxattr(f, FILL_XATTR, buf, lo, 0))
		return 0;
	ntfs_volume_get_info(g_vol, &info);
	return info.free_clusters == before ? lo : 0;
}

/*
 * Leave the free space in GAP_CLUSTERS-cluster gaps, as test_fragmented_rewrite
 * does with larger ones: GAP_FILES small files, the rest of the volume filled,
 * then every other small file deleted on a fresh mount. @buf is 1 MiB of
 * scratch.
 */
static void fragment_free_space(unsigned char *buf)
{
	const size_t small = GAP_CLUSTERS * FRAG_CS;
	ntfs_inode_t *root = NULL, *dir = NULL, *f;
	char name[32];
	int i;

	CHECK_EQ(ntfs_volume_root(g_vol, &root), 0, "root");
	CHECK_EQ(ntfs_mkdir(root, "gaps", 0755, &dir), 0, "mkdir /gaps");
	if (!root || !dir)
		goto out;
	memset(buf, 'g', small);
	for (i = 0; i < GAP_FILES; i++) {
		snprintf(name, sizeof(name), "g%02d", i);
		if (ntfs_create(dir, name, 0100644, &f)) {
			CHECK(!"create a small file");
			break;
		}
		CHECK_EQ(ntfs_write(f, buf, small, 0), small, "write a small file");
		ntfs_inode_put(f);
	}
	fill_volume(dir, buf);
	ntfs_inode_put(dir);
	dir = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, "sync after the fill");
	umount_scratch();

	CHECK_EQ(mount_scratch(), 0, "remount");
	if (!g_vol)
		return;
	CHECK_EQ(ntfs_volume_root(g_vol, &root), 0, "root");
	CHECK_EQ(ntfs_lookup(root, "gaps", &dir), 0, "lookup /gaps");
	if (!dir)
		goto out;
	for (i = 0; i < GAP_FILES; i += 2) {
		snprintf(name, sizeof(name), "g%02d", i);
		CHECK_EQ(ntfs_unlink(dir, name), 0, "unlink every other small file");
	}
out:
	if (dir)
		ntfs_inode_put(dir);
	if (root)
		ntfs_inode_put(root);
}

static void rewrite_update_fails(const char *tag, const char *file, bool fragment,
				 unsigned seed)
{
	const size_t mib = 1u << 20;
	unsigned char *buf = malloc(mib), *rb = malloc(mib), *orig = malloc(mib);
	unsigned char *blk = malloc(CB_SIZE);
	struct ntfs_volume_info info;
	ntfs_inode_t *f;
	uint64_t fill, lo = 0, hi = 0;

	if (!buf || !rb || !orig || !blk) {
		CHECK(!"out of memory");
		goto free;
	}
	if (copy_fixture("compressed-sparse.img") != 0) {
		fprintf(stderr, "SKIP %s: no fixture\n", tag);
		goto free;
	}
	g_refusing = true;
	if (mount_scratch()) { failures++; checks++; goto out; }
	ntfs_volume_get_info(g_vol, &info);
	CHECK_EQ(info.cluster_size, FRAG_CS, "the fixture's cluster size");
	if (info.cluster_size != FRAG_CS)
		goto out;

	f = open_file("compressed", file);
	CHECK(f != NULL);
	if (!f)
		goto out;
	CHECK_EQ(read_exact(f, orig, mib, 0), 0, label(tag, "read the file"));
	fill = fill_base_record(f, buf);
	ntfs_inode_put(f);
	checks++;
	if (!fill) {
		failures++;
		fprintf(stderr, "FAIL %s: the base record could not be filled\n", tag);
		goto out;
	}
	if (fragment)
		fragment_free_space(buf);
	if (!g_vol)
		goto out;
	CHECK_EQ(ntfs_volume_sync(g_vol), 0, label(tag, "sync before the rewrite"));
	umount_scratch();

	/* On a fresh mount, so that nothing has read the free records' pages. */
	CHECK_EQ(mount_scratch(), 0, label(tag, "remount"));
	if (!g_vol)
		goto out;
	CHECK_EQ(mft_range(&lo, &hi), 0, label(tag, "find $MFT on the image"));
	if (seed)
		fill_random(blk, CB_SIZE, seed);
	else
		memset(blk, 0, CB_SIZE);
	rewrite_refused(&(struct refused_rewrite){ .tag = tag, .file = file,
			.blk = blk, .want = -EIO, .read_only = true,
			.refuse_lo = lo, .refuse_hi = hi }, orig, rb);
	printf("  %s: \"fill\" %llu bytes, %d read(s) of $MFT refused, the first at "
	       "byte %llu\n", tag, (unsigned long long)fill, refused_reads,
	       (unsigned long long)refused_read_off);
	checks++;
	if (!refused_reads) {
		failures++;
		fprintf(stderr, "FAIL %s: no read of $MFT was refused during the rewrite: "
			"its mapping pairs fitted, and this case tested nothing\n", tag);
	}
out:
	g_refusing = false;
	umount_scratch();
	unlink(scratch);
free:
	free(buf);
	free(rb);
	free(orig);
	free(blk);
}

static void test_split_run_update_fails(void)
{
	rewrite_update_fails("split_run_update_fails", "random-1m.bin", false, 34);
}

/* Seed 0 writes zeroes, which the U7 path punches without a new block. */
static void test_zero_block_update_fails(void)
{
	rewrite_update_fails("zero_block_update_fails", "random-1m.bin", false, 0);
}

static void test_fragmented_update_fails(void)
{
	rewrite_update_fails("fragmented_update_fails", "text-1m.txt", true, 35);
}

int main(void)
{
	static const struct { void (*fn)(void); const char *name; } groups[] = {
		{ test_create_in_compressed_directory, "create_in_compressed_directory" },
		{ test_overwrite_in_place,	 "overwrite_in_place" },
		{ test_cb_boundary,		 "cb_boundary" },
		{ test_extend,			 "extend" },
		{ test_append,			 "append" },
		{ test_zero_block,		 "zero_block" },
		{ test_size_change_refused,	 "size_change_refused" },
		{ test_resident_compressed_grow, "resident_compressed_grow" },
		{ test_sparse_fixtures,		 "sparse_fixtures" },
		{ test_sparse_write,		 "sparse_write" },
		{ test_encrypted_refused,	 "encrypted_refused" },
		{ test_fragmented_rewrite,	 "fragmented_rewrite" },
		{ test_full_volume_rewrite,	 "full_volume_rewrite" },
		{ test_rewrite_write_error,	 "rewrite_write_error" },
		{ test_split_run_update_fails,	 "split_run_update_fails" },
		{ test_zero_block_update_fails,	 "zero_block_update_fails" },
		{ test_fragmented_update_fails,	 "fragmented_update_fails" },
	};

	/* Per-group counts, so a group that silently stopped checking anything
	 * (a missing fixture, an early return) is visible in the log instead of
	 * hiding behind a passing total. */
	for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		int before = checks, failed = failures;

		groups[i].fn();
		printf("%-26s %3d checks%s\n", groups[i].name, checks - before,
		       failures > failed ? " (FAILED)" : "");
	}
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
