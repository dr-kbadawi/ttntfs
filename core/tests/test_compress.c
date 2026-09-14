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
 *   extend               Writing past EOF. Two real bugs live here; see below.
 *   zero_block           Writing zeroes over a block that holds data. One real
 *                        bug; see below.
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
 *
 * Bugs found while writing this, pinned here as the behaviour that exists
 * rather than the behaviour that is wanted. Each assertion that encodes a bug
 * says so in a comment beginning "BUG:".
 *
 *   1. ntfs_compress_write() grows the attribute only when the write passes
 *      ni->allocated_size, not ni->data_size. A compressed file's allocation is
 *      rounded up to the compression block, so a write into that rounding gap
 *      returns success, rewrites the block, and leaves i_size alone: the bytes
 *      are on disk and unreachable.
 *   2. Nothing on the compressed write path advances initialized_size. When the
 *      write does pass allocated_size, ntfs_attr_expand() moves data_size but
 *      initialized_size stays at the old EOF, so everything written past the
 *      old end of file reads back as zeroes -- after a remount too.
 *   3. ntfs_write_cb() returns early when a block compresses to nothing but
 *      zeroes, before it punches the old block's clusters out. Zeroing a range
 *      that held data leaves the old data on disk and readable.
 *
 * All three are data loss of the quiet kind: the write reports success.
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
 * no longer describe the allocation, a compressed_size that disagrees with the
 * run list. Never without -n.
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

static int mount_scratch(void)
{
	struct ntfs_mount_options o = { .flags = 0, .uid = 0, .gid = 0,
					.fmask = 022, .dmask = 022 };
	struct ntfs_volume_info info;
	int err;

	g_dev = ntfs_bdev_open_path(scratch, false);
	if (!g_dev)
		return -ENOENT;
	err = ntfs_mount(g_dev, &o, &g_vol);
	if (err) {
		ntfs_bdev_close(g_dev);
		g_dev = NULL;
		return err;
	}
	ntfs_volume_get_info(g_vol, &info);
	if (info.read_only) {
		fprintf(stderr, "FAIL: fixture mounted read-only (reason %d); "
			"nothing below would have been exercised\n", info.ro_reason);
		failures++; checks++;
	}
	return 0;
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
	 * still fits inside it.
	 *
	 * BUG: ntfs_compress_write() expands the attribute only when
	 * pos + count > ni->allocated_size, so nothing grows i_size here. The
	 * call reports 8192 bytes written, the block is re-encoded and written
	 * to the disk, and the bytes are unreachable for ever after.
	 * The right test, once this is fixed, is size == 110592 and the tail
	 * reading back as 'Z'.
	 */
	memset(patch, 'Z', 8192);
	CHECK_EQ(ntfs_write(ni, patch, 8192, size), 8192, "write 8 KiB at EOF");
	CHECK_EQ(data_size(ni), size, "BUG: i_size unchanged by a write past EOF "
		 "that lands inside the compression-block rounding");
	CHECK_EQ(ntfs_read(ni, back, 8192, size), 0, "BUG: the bytes just written "
		 "are past i_size and read back as EOF");

	/* What was already in the file must at least have survived. */
	CHECK_EQ(read_exact(ni, back, size, 0), 0, "read back after lost extend");
	compare("existing data after a lost extend", back, ref, size);

	/*
	 * (b) past the allocation this time, so ntfs_attr_expand() runs and
	 * i_size does move.
	 *
	 * BUG: nothing on this path advances initialized_size. It stays at the
	 * old EOF, so every byte past it -- including the ones just written --
	 * reads back as zero, before and after a remount. ntfsck is happy,
	 * because an attribute whose initialized_size is below its data_size is
	 * perfectly legal; it just does not mean what the writer intended.
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
		/* ...and everything above it, written or not, is zeroes. */
		memset(back, 0xff, 4096);
		CHECK_EQ(read_exact(ni, back, 4096, far), 0, "read back the new data");
		CHECK(all_zero(back, 4096));	/* BUG: expected 4096 bytes of 'Q'.
						 * initialized_size is never advanced
						 * on the compressed write path, so
						 * everything past the old EOF reads
						 * as a hole. */
		ntfs_inode_put(ni);
	}
out:
	free(ref); free(back); free(patch);
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
	uint64_t size;

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

	CHECK_EQ(ntfs_write(ni, zeroes, CB_SIZE, 0), CB_SIZE, "zero the first block");
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
		 * BUG: this should be the zeroes that were written. What comes
		 * back is the block's previous contents, byte for byte. The
		 * correct assertion, once ntfs_write_cb() punches the hole
		 * before its all-zeroes early return, is
		 *   first_diff(back, zeroes, CB_SIZE) == -1.
		 */
		CHECK_EQ(first_diff(back, ref, CB_SIZE), -1,
			 "BUG: 64 KiB of zeroes written over a block holding data "
			 "left the old data in place");
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

int main(void)
{
	static const struct { void (*fn)(void); const char *name; } groups[] = {
		{ test_overwrite_in_place,	 "overwrite_in_place" },
		{ test_cb_boundary,		 "cb_boundary" },
		{ test_extend,			 "extend" },
		{ test_zero_block,		 "zero_block" },
		{ test_size_change_refused,	 "size_change_refused" },
		{ test_resident_compressed_grow, "resident_compressed_grow" },
		{ test_sparse_fixtures,		 "sparse_fixtures" },
		{ test_sparse_write,		 "sparse_write" },
		{ test_encrypted_refused,	 "encrypted_refused" },
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
