// SPDX-License-Identifier: GPL-2.0
/*
 * test_links.c - hard links, symlinks, timestamps and permissions.
 *
 * All four were implemented and none was exercised by anything. What each
 * group is here to catch:
 *
 *   hard links   A link count that is one too high leaves the MFT record and
 *                its clusters allocated after the last name is gone: a silent
 *                leak. One too low frees a record another name still points
 *                at: silent data loss, and the loss surfaces later, on a
 *                different file, when the record is reused. Neither shows up
 *                in a read-back of the file just touched, so every mutating
 *                case here ends in ntfsck.
 *   symlinks     Written as WSL reparse points (IO_REPARSE_TAG_LX_SYMLINK).
 *                The target is the only copy of the information; if readlink
 *                loses or truncates it the link is unrecoverable, and nothing
 *                else on the volume looks wrong.
 *   timestamps   NTFS counts 100 ns ticks from 1601 and Unix counts seconds
 *                from 1970. A conversion that is off by the epoch constant
 *                puts every file 369 years away, and a conversion that is off
 *                by a factor of ten is not obviously wrong on a screen.
 *   permissions  noowners: the mode the caller sees comes from the mount
 *                options, not from the volume. If fmask/dmask stop being
 *                applied, files quietly become world-writable.
 *
 * Verified against the implementation before being asserted: core/vfs/namei.c
 * (__ntfs_link, ntfs_delete, __ntfs_create), core/vfs/api.c (mode_of,
 * fill_attr, ntfs_setattr, ntfs_symlink, ntfs_readlink), core/vfs/file.c
 * (ntfs_vfs_setattr), core/ntfs/reparse.c and core/ntfs/time.h. Where the port
 * refuses something, or does something surprising, the comment says so.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include "ntfscore.h"
#include "ntfsport/bdev.h"
/*
 * The conversion itself, so the known values below are checked directly and
 * not only through a mount. Included by relative path on purpose: core/ntfs is
 * NOT on this target's include path, because a time.h there would shadow the
 * system <time.h> that linux/time64.h needs.
 */
#include <linux/types.h>
#include <asm/byteorder.h>
#include <linux/math64.h>
#include "../ntfs/time.h"

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

static const char *images_dir(void)
{
	const char *d = getenv("NTFS_IMAGES");
	return d && *d ? d : "tools/images";
}

/* ---- a scratch copy of a fixture ------------------------------------- */

/*
 * Each test gets its own copy under its own name: the fixtures are shared with
 * every other test and a repair run, or a half-written image, would be a
 * measurement of the repair rather than of the driver.
 */
static char scratch[1024];

static int copy_fixture(const char *tag)
{
	char src[1024];
	int in, out;
	char buf[1 << 16];
	ssize_t n;

	snprintf(src, sizeof(src), "%s/basic-4k.img", images_dir());
	snprintf(scratch, sizeof(scratch), "/tmp/ttntfs-links-%d-%s.img",
		 (int)getpid(), tag);
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
 * ntfsck catches what a read-back cannot: a link count that disagrees with the
 * number of FILE_NAME attributes, an index entry with no attribute behind it,
 * an MFT record still marked in use with nothing referencing it. Always -n:
 * repairing the image under test would measure the repair.
 */
static void fsck_clean(const char *what)
{
	const char *ck = getenv("NTFS_NTFSCK");
	char cmd[2048];
	int rc;

	if (!ck || !*ck || access(ck, X_OK) != 0) {
		fprintf(stderr, "SKIP fsck(%s): NTFS_NTFSCK not usable\n", what);
		return;
	}
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' >/dev/null 2>&1", ck, scratch);
	rc = system(cmd);
	checks++;
	if (rc != 0) {
		failures++;
		fprintf(stderr, "FAIL ntfsck -n after %s: exit %d (image left at %s)\n",
			what, rc, scratch);
		/* Show what it found; the image is about to be unlinked. */
		snprintf(cmd, sizeof(cmd), "'%s' -n '%s' 2>&1 | tail -20 1>&2", ck, scratch);
		rc = system(cmd);
		(void)rc;
	}
}

/* ---- mount / unmount ------------------------------------------------- */

struct vol {
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root;
};

static int vol_up_flags(struct vol *v, uint32_t flags, uint16_t fmask, uint16_t dmask,
			uid_t uid, gid_t gid);

static int vol_up(struct vol *v, uint16_t fmask, uint16_t dmask,
		  uid_t uid, gid_t gid)
{
	return vol_up_flags(v, 0, fmask, dmask, uid, gid);
}

static int vol_up_flags(struct vol *v, uint32_t flags, uint16_t fmask, uint16_t dmask,
			uid_t uid, gid_t gid)
{
	struct ntfs_mount_options o = { .flags = flags, .uid = uid, .gid = gid,
					.fmask = fmask, .dmask = dmask };
	int err;

	memset(v, 0, sizeof(*v));
	v->dev = ntfs_bdev_open_path(scratch, false);
	if (!v->dev)
		return -ENOENT;
	err = ntfs_mount(v->dev, &o, &v->vol);
	if (err) {
		ntfs_bdev_close(v->dev);
		v->dev = NULL;
		return err;
	}
	err = ntfs_volume_root(v->vol, &v->root);
	if (err) {
		ntfs_unmount(v->vol);
		ntfs_bdev_close(v->dev);
		v->dev = NULL;
		return err;
	}
	return 0;
}

static void vol_down(struct vol *v)
{
	if (!v->dev)
		return;
	ntfs_inode_put(v->root);
	ntfs_unmount(v->vol);
	ntfs_bdev_close(v->dev);
	v->dev = NULL;
}

/* ---- 1. hard links: the count, the shared inode, the shared data ----- */

/*
 * The whole point of a hard link is that the two names are one file. If they
 * ever stop sharing an inode number the caller's inode cache splits in two and
 * a write through one name is invisible through the other.
 */
static void test_hard_links(void)
{
	struct vol v;
	ntfs_inode_t *a = NULL, *b = NULL;
	struct ntfs_attr aa, ab;
	char buf[32];
	uint64_t ino;

	if (copy_fixture("hardlinks") != 0) {
		fprintf(stderr, "SKIP test_hard_links: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_hard_links: mount failed\n");
		unlink(scratch);
		return;
	}

	CHECK(ntfs_create(v.root, "one.txt", 0100644, &a) == 0);
	if (!a) goto out;
	CHECK(ntfs_write(a, "hello", 5, 0) == 5);
	CHECK(ntfs_getattr(a, &aa) == 0);
	CHECK(aa.nlink == 1);
	ino = aa.inode_no;

	/* Second name. */
	CHECK(ntfs_link(a, v.root, "two.txt") == 0);
	CHECK(ntfs_getattr(a, &aa) == 0);
	CHECK_MSG(aa.nlink == 2, "nlink after link is %u, expected 2", aa.nlink);

	CHECK(ntfs_lookup(v.root, "two.txt", &b) == 0);
	if (b) {
		CHECK(ntfs_getattr(b, &ab) == 0);
		CHECK_MSG(ab.inode_no == ino,
			  "the two names are different inodes: %llu vs %llu",
			  (unsigned long long)ab.inode_no, (unsigned long long)ino);
		CHECK(ab.nlink == 2);
		CHECK(ab.size == 5);
		memset(buf, 0, sizeof(buf));
		CHECK(ntfs_read(b, buf, 5, 0) == 5);
		CHECK(memcmp(buf, "hello", 5) == 0);

		/* A write through the new name must be visible through the old. */
		CHECK(ntfs_write(b, "HELLO", 5, 0) == 5);
		memset(buf, 0, sizeof(buf));
		CHECK(ntfs_read(a, buf, 5, 0) == 5);
		CHECK_MSG(memcmp(buf, "HELLO", 5) == 0,
			  "the two names do not share data: read '%.5s'", buf);
		ntfs_inode_put(b);
		b = NULL;
	}

	/* Drop the first name. The file must still be there under the second. */
	CHECK(ntfs_unlink(v.root, "one.txt") == 0);
	CHECK(ntfs_getattr(a, &aa) == 0);
	CHECK_MSG(aa.nlink == 1, "nlink after unlinking one of two names is %u, expected 1",
		  aa.nlink);
	CHECK(ntfs_lookup(v.root, "one.txt", &b) == -ENOENT);
	CHECK(ntfs_lookup(v.root, "two.txt", &b) == 0);
	if (b) {
		memset(buf, 0, sizeof(buf));
		CHECK_MSG(ntfs_read(b, buf, 5, 0) == 5,
			  "the surviving name is unreadable after the other was removed");
		CHECK(memcmp(buf, "HELLO", 5) == 0);
		ntfs_inode_put(b);
		b = NULL;
	}
	ntfs_inode_put(a);
	a = NULL;
	vol_down(&v);
	fsck_clean("hard link create + one unlink");

	/* The count and the data must come back off disk, not out of the cache. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_hard_links: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	CHECK(ntfs_lookup(v.root, "two.txt", &b) == 0);
	if (b) {
		CHECK(ntfs_getattr(b, &ab) == 0);
		CHECK_MSG(ab.nlink == 1, "nlink off disk is %u, expected 1", ab.nlink);
		CHECK_MSG(ab.inode_no == ino, "inode number changed across a remount");
		memset(buf, 0, sizeof(buf));
		CHECK(ntfs_read(b, buf, 5, 0) == 5);
		CHECK(memcmp(buf, "HELLO", 5) == 0);
		ntfs_inode_put(b);
		b = NULL;
	}
	/* Last name: now the file really goes. */
	CHECK(ntfs_unlink(v.root, "two.txt") == 0);
	CHECK(ntfs_lookup(v.root, "two.txt", &b) == -ENOENT);
out:
	if (a) ntfs_inode_put(a);
	vol_down(&v);
	/* The MFT record must be free and nothing must still point at it. */
	fsck_clean("hard link last unlink");
	unlink(scratch);
	printf("test_hard_links\n");
}

/* ---- 2. what ntfs_link refuses -------------------------------------- */

/*
 * A hard link to a directory makes the index a cycle: no fsck can untangle it
 * afterwards, so the refusal is the safety property, not an inconvenience.
 */
static void test_hard_link_refusals(void)
{
	struct vol v;
	ntfs_inode_t *d = NULL, *f = NULL, *l = NULL;
	char buf[16];
	size_t n;

	if (copy_fixture("linkrefuse") != 0) {
		fprintf(stderr, "SKIP test_hard_link_refusals: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_hard_link_refusals: mount failed\n");
		unlink(scratch);
		return;
	}
	CHECK(ntfs_mkdir(v.root, "dir1", 0755, &d) == 0);
	CHECK(ntfs_create(v.root, "file1", 0100644, &f) == 0);
	if (!d || !f) goto out;

	CHECK_MSG(ntfs_link(d, v.root, "dir2") == -EPERM,
		  "a hard link to a directory was not refused");
	CHECK(ntfs_lookup(v.root, "dir2", &l) == -ENOENT);

	CHECK_MSG(ntfs_link(f, v.root, "file1") == -EEXIST,
		  "linking over an existing name was not refused");
	/* The target of the link must be a directory. */
	CHECK(ntfs_link(f, f, "x") == -ENOTDIR);
	/* Empty and path-shaped names are not single components. */
	CHECK(ntfs_link(f, v.root, "") == -EINVAL);
	CHECK(ntfs_link(f, v.root, "a/b") == -EINVAL);
	CHECK(ntfs_link(f, v.root, ".") == -EINVAL);

	/* And the two remove calls do not substitute for each other. */
	CHECK(ntfs_rmdir(v.root, "file1") == -ENOTDIR);
	CHECK(ntfs_unlink(v.root, "dir1") == -EISDIR);

	/* readlink only answers for symlinks. */
	n = 0;
	CHECK(ntfs_readlink(f, buf, sizeof(buf), &n) == -EINVAL);

	/* Nothing above may have changed the link count. */
	{
		struct ntfs_attr at;
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.nlink == 1, "a refused link still bumped nlink to %u", at.nlink);
	}
out:
	if (d) ntfs_inode_put(d);
	if (f) ntfs_inode_put(f);
	vol_down(&v);
	fsck_clean("refused links");
	unlink(scratch);
	printf("test_hard_link_refusals\n");
}

/* ---- 3. many links, up to the cap ----------------------------------- */

/* Extra names the cap allows on top of the one the file was created with. */
#define MANY_LINKS (NTFS_LINK_MAX - 1)

/*
 * NTFS_LINK_MAX is Windows' limit, not an on-disk one: link_count is a __le16
 * and the FILE_NAME attributes spill into extents, so the structure holds
 * 65535, and upstream Linux fs/ntfs enforces nothing at all. Microsoft
 * documents 1023 links creatable with CreateHardLink on top of the name the
 * file was created with, so 1024 names in total, and Windows fails past it. A
 * volume this driver writes has to stay usable on Windows, so the port
 * enforces the same number and returns -EMLINK.
 *
 * (The project previously carried a bare "1023" in the FSKit pathconf block
 * and nothing enforced it anywhere. 1023 is what Microsoft says can be
 * *added*; the count a file carries, and what pathconf reports, is 1024.)
 *
 * This is also the only case that forces the FILE_NAME attributes out of the
 * base MFT record and into extents via $ATTRIBUTE_LIST, which is exactly where
 * a link count and the real number of names drift apart. Hence the fsck.
 */
static void test_many_links(void)
{
	struct vol v;
	ntfs_inode_t *f = NULL, *l = NULL;
	struct ntfs_attr at;
	char nm[32];
	uint64_t ino = 0;
	int i, made = 0, err;

	if (copy_fixture("manylinks") != 0) {
		fprintf(stderr, "SKIP test_many_links: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_many_links: mount failed\n");
		unlink(scratch);
		return;
	}
	CHECK(ntfs_create(v.root, "many.bin", 0100644, &f) == 0);
	if (!f) goto out;
	CHECK(ntfs_write(f, "payload", 7, 0) == 7);
	ino = ntfs_inode_number(f);

	for (i = 0; i < MANY_LINKS; i++) {
		snprintf(nm, sizeof(nm), "ln%04d", i);
		err = ntfs_link(f, v.root, nm);
		if (err) {
			fprintf(stderr, "  link #%d (%s) failed: %d\n", i + 2, nm, err);
			break;
		}
		made++;
	}
	CHECK_MSG(made == MANY_LINKS,
		  "only %d of %d extra links were created; the cap bites early",
		  made, MANY_LINKS);
	CHECK(ntfs_getattr(f, &at) == 0);
	CHECK_MSG(at.nlink == (uint32_t)made + 1,
		  "nlink is %u after %d extra links, expected %d",
		  at.nlink, made, made + 1);
	CHECK_MSG(at.nlink == NTFS_LINK_MAX,
		  "the file should now be sitting exactly on the cap: nlink %u, cap %d",
		  at.nlink, NTFS_LINK_MAX);

	/*
	 * The name after the cap is refused, and refused without a trace: the
	 * count must not move and the name must not appear in the directory.
	 * A half-done link here is the leak this whole group exists to catch.
	 */
	CHECK_MSG(ntfs_link(f, v.root, "one-too-many") == -EMLINK,
		  "the %dth name was not refused with -EMLINK", NTFS_LINK_MAX + 1);
	CHECK(ntfs_getattr(f, &at) == 0);
	CHECK_MSG(at.nlink == NTFS_LINK_MAX,
		  "the refused link still moved nlink to %u", at.nlink);
	CHECK(ntfs_lookup(v.root, "one-too-many", &l) == -ENOENT);

	/*
	 * A file sitting on the cap must still be renameable. rename() adds the
	 * new name before dropping the old one, so a cap checked inside
	 * __ntfs_link() would refuse this even though the number of names does
	 * not change. Rename one of the links and put it back.
	 */
	CHECK_MSG(ntfs_rename(v.root, "ln0000", v.root, "renamed") == 0,
		  "renaming a name of a file at the cap was refused");
	CHECK(ntfs_getattr(f, &at) == 0);
	CHECK_MSG(at.nlink == NTFS_LINK_MAX,
		  "rename at the cap changed nlink to %u", at.nlink);
	CHECK(ntfs_rename(v.root, "renamed", v.root, "ln0000") == 0);

	ntfs_inode_put(f);
	f = NULL;
	vol_down(&v);
	fsck_clean("hard links up to the cap");

	/* The count must be right off disk too. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_many_links: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	CHECK(ntfs_lookup(v.root, "ln0000", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK_MSG(at.nlink == (uint32_t)made + 1,
			  "nlink off disk is %u, expected %d", at.nlink, made + 1);
		CHECK(at.inode_no == ino);
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Take them all off again. The count must come back down to 1, not to
	 * some remainder: a residue here is a record that will never be freed. */
	for (i = 0; i < made; i++) {
		snprintf(nm, sizeof(nm), "ln%04d", i);
		if (ntfs_unlink(v.root, nm) != 0) {
			CHECK_MSG(0, "unlink of %s failed", nm);
			break;
		}
	}
	CHECK(ntfs_lookup(v.root, "many.bin", &f) == 0);
	if (f) {
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.nlink == 1, "nlink after removing every extra name is %u, expected 1",
			  at.nlink);
	}
out:
	if (f) ntfs_inode_put(f);
	vol_down(&v);
	fsck_clean("every extra link removed again");
	unlink(scratch);
	printf("test_many_links (%d extra links, cap %d)\n", made, NTFS_LINK_MAX);
}

/* ---- 4. symlinks ---------------------------------------------------- */

static void check_target(ntfs_inode_t *l, const char *want, const char *what)
{
	char buf[2048];
	size_t n = 0;
	int err;

	err = ntfs_readlink(l, buf, sizeof(buf), &n);
	checks++;
	if (err) {
		failures++;
		fprintf(stderr, "FAIL readlink(%s): %d\n", what, err);
		return;
	}
	checks++;
	if (n != strlen(want) || memcmp(buf, want, n) != 0) {
		failures++;
		fprintf(stderr, "FAIL readlink(%s): got '%.*s' (%zu bytes), expected '%s'\n",
			what, (int)n, buf, n, want);
	}
}

/*
 * Symlinks are written as WSL reparse points (IO_REPARSE_TAG_LX_SYMLINK,
 * core/ntfs/reparse.c). The target string in the reparse attribute is the only
 * record of where the link points, and nothing else on the volume can be used
 * to reconstruct it, so a readlink that truncates or mangles it destroys the
 * link with no other symptom.
 */

/* The reparse tag actually on disk for @name in the root, or 0. Read from the
 * image, not through the driver, so it tests what was written rather than what
 * the driver believes it wrote. */
static uint32_t disk_reparse_tag(const char *name)
{
	const char *info = getenv("NTFS_NTFSINFO");
	char cmd[PATH_MAX + 300], line[256];
	uint32_t tag = 0;
	FILE *p;

	if (!info || !*info || access(info, X_OK) != 0)
		return 0;
	snprintf(cmd, sizeof(cmd),
		 "%s -F /%s -v %s 2>/dev/null | grep -A40 'REPARSE_POINT (0xc0)' | "
		 "grep -m1 'Reparse tag:' | grep -oE '0x[0-9a-fA-F]+'",
		 info, name, scratch);
	p = popen(cmd, "r");
	if (!p)
		return 0;
	if (fgets(line, sizeof(line), p))
		tag = (uint32_t)strtoul(line, NULL, 16);
	pclose(p);
	return tag;
}

/*
 * Which reparse tag a symlink gets, and that it survives a remount either way.
 *
 * Finding 20. Upstream writes only IO_REPARSE_TAG_LX_SYMLINK, which Windows
 * refuses to follow and Linux ntfs3 cannot read. IO_REPARSE_TAG_SYMLINK is
 * followed by every reader there is, so it is the default whenever the target
 * can be expressed in it. Targets Windows cannot name -- : * ? " < > | -- and a
 * literal backslash, which the native form cannot tell from a separator, keep
 * the WSL tag. That is the per-link rule Microsoft's own DrvFs applies. The
 * wsl_symlinks mount flag forces the old behaviour for every link.
 *
 * Two things this pins that the first version of the writer got wrong or
 * nearly wrong: a literal backslash must NOT go native (it came back as '/'),
 * and getattr must report the target length after a remount for BOTH tags,
 * because ni->target used to be filled only for the WSL tag and native links
 * stat()ed at 0 bytes.
 */
static void test_symlink_tag_choice(void)
{
	static const struct {
		const char *name, *target;
		uint32_t want_tag;		/* with default flags */
	} cases[] = {
		{ "n-rel",     "target.txt",       0xA000000Cu },	/* native */
		{ "n-deep",    "dir/sub/f.txt",    0xA000000Cu },
		{ "n-up",      "../sib/y.txt",     0xA000000Cu },
		{ "n-unixabs", "/usr/bin/foo",     0xA000000Cu },	/* ntfs3 convention */
		{ "n-utf8",    "caf\xc3\xa9/\xe6\x97\xa5.txt", 0xA000000Cu },
		{ "w-colon",   "a:b",              0xA000001Du },	/* WSL fallback */
		{ "w-star",    "x*y",              0xA000001Du },
		{ "w-quote",   "say\"hi\"",        0xA000001Du },
		{ "w-bslash",  "a\\b",             0xA000001Du },	/* the near miss */
	};
	const size_t N = sizeof(cases) / sizeof(cases[0]);
	int pass;

	printf("test_symlink_tag_choice\n");

	/* pass 0: default flags, per-link choice. pass 1: wsl_symlinks forced. */
	for (pass = 0; pass < 2; pass++) {
		uint32_t flags = pass ? NTFS_MOUNT_WSL_SYMLINKS : 0;
		struct vol v;
		ntfs_inode_t *l = NULL;
		size_t i;

		if (copy_fixture(pass ? "tagchoice-wsl" : "tagchoice") != 0) {
			fprintf(stderr, "SKIP test_symlink_tag_choice: no fixture\n");
			return;
		}
		if (vol_up_flags(&v, flags, 022, 022, 501, 20)) {
			fprintf(stderr, "SKIP test_symlink_tag_choice: mount failed\n");
			unlink(scratch);
			return;
		}
		for (i = 0; i < N; i++) {
			CHECK_MSG(ntfs_symlink(v.root, cases[i].name, cases[i].target, &l) == 0,
				  "symlink(%s) failed", cases[i].name);
			if (l) { ntfs_inode_put(l); l = NULL; }
		}
		vol_down(&v);

		/* What is on disk, read independently of the driver. */
		for (i = 0; i < N; i++) {
			uint32_t want = pass ? 0xA000001Du : cases[i].want_tag;
			uint32_t got = disk_reparse_tag(cases[i].name);

			if (!got) {
				fprintf(stderr, "SKIP tag(%s): ntfsinfo unavailable\n", cases[i].name);
				continue;
			}
			CHECK_MSG(got == want, "%s%s: on-disk tag 0x%08x, expected 0x%08x",
				  cases[i].name, pass ? " (wsl_symlinks)" : "", got, want);
		}

		/* Remount: a cached read proves nothing. Target and size must both
		 * survive, for both tags. */
		if (vol_up_flags(&v, flags, 022, 022, 501, 20)) {
			fprintf(stderr, "FAIL test_symlink_tag_choice: remount failed\n");
			failures++; checks++;
			unlink(scratch);
			return;
		}
		for (i = 0; i < N; i++) {
			struct ntfs_attr at;
			char buf[256];
			size_t n = 0;

			CHECK_MSG(ntfs_lookup(v.root, cases[i].name, &l) == 0,
				  "lookup(%s) after remount", cases[i].name);
			if (!l)
				continue;
			CHECK(ntfs_getattr(l, &at) == 0);
			CHECK_MSG((at.mode & S_IFMT) == S_IFLNK, "%s is not a symlink after remount",
				  cases[i].name);
			CHECK_MSG(ntfs_readlink(l, buf, sizeof(buf), &n) == 0, "readlink(%s)", cases[i].name);
			CHECK_MSG(n == strlen(cases[i].target) && !memcmp(buf, cases[i].target, n),
				  "readlink(%s): got '%.*s', expected '%s'", cases[i].name, (int)n, buf,
				  cases[i].target);
			CHECK_MSG(at.size == (int64_t)strlen(cases[i].target),
				  "%s: size %lld after remount, expected %zu (finding 20: native links "
				  "used to stat at 0)", cases[i].name, (long long)at.size,
				  strlen(cases[i].target));
			ntfs_inode_put(l); l = NULL;
		}
		vol_down(&v);
		fsck_clean("symlink tag choice");
		unlink(scratch);
	}
}

static void test_symlinks(void)
{
	struct vol v;
	ntfs_inode_t *l = NULL, *f = NULL;
	struct ntfs_attr at;
	char big[PATH_MAX + 8], buf[8];
	size_t n;

	if (copy_fixture("symlinks") != 0) {
		fprintf(stderr, "SKIP test_symlinks: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_symlinks: mount failed\n");
		unlink(scratch);
		return;
	}

	/* Absolute target. */
	CHECK(ntfs_symlink(v.root, "abs.lnk", "/etc/hosts", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK(at.type == NTFS_ITEM_SYMLINK);
		CHECK_MSG((at.mode & S_IFMT) == S_IFLNK, "mode %o is not S_IFLNK", at.mode);
		/* The reported size is the target length, which is what a
		 * caller sizing a readlink buffer relies on. */
		CHECK_MSG(at.size == strlen("/etc/hosts"),
			  "symlink size is %llu, expected the target length %zu",
			  (unsigned long long)at.size, strlen("/etc/hosts"));
		CHECK(at.nlink == 1);
		check_target(l, "/etc/hosts", "abs.lnk");
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Relative target: must be stored verbatim, not resolved or rebased. */
	CHECK(ntfs_symlink(v.root, "rel.lnk", "../sib/y.txt", &l) == 0);
	if (l) { check_target(l, "../sib/y.txt", "rel.lnk"); ntfs_inode_put(l); l = NULL; }

	/* Dangling: nothing resolves the target at create time, and nothing
	 * should. A link to a name that does not exist yet is legal. */
	CHECK(ntfs_symlink(v.root, "dangle.lnk", "no/such/file", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK(at.type == NTFS_ITEM_SYMLINK);
		check_target(l, "no/such/file", "dangle.lnk");
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Non-ASCII survives the UTF-8 -> UTF-16 -> UTF-8 round trip through
	 * the reparse attribute. */
	CHECK(ntfs_symlink(v.root, "utf8.lnk", "caf\xc3\xa9/\xe2\x98\x83.txt", &l) == 0);
	if (l) { check_target(l, "caf\xc3\xa9/\xe2\x98\x83.txt", "utf8.lnk"); ntfs_inode_put(l); l = NULL; }

	/*
	 * A backslash in a WSL target stays a backslash. ntfs_readlink only
	 * rewrites "\" to "/" (and strips "\??\" and a drive letter) on the
	 * OTHER path, for a Windows IO_REPARSE_TAG_SYMLINK written by Windows.
	 * Applying that rewrite here would corrupt a legitimate POSIX name.
	 */
	CHECK(ntfs_symlink(v.root, "bs.lnk", "a\\b", &l) == 0);
	if (l) { check_target(l, "a\\b", "bs.lnk"); ntfs_inode_put(l); l = NULL; }

	/* A symlink pointing at a real file is still a symlink: it is not
	 * followed, and its size is the target's length, not the file's. */
	CHECK(ntfs_create(v.root, "real.txt", 0100644, &f) == 0);
	if (f) { CHECK(ntfs_write(f, "DATA", 4, 0) == 4); ntfs_inode_put(f); f = NULL; }
	CHECK(ntfs_symlink(v.root, "to-real.lnk", "real.txt", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK(at.type == NTFS_ITEM_SYMLINK);
		CHECK_MSG(at.size == strlen("real.txt"),
			  "symlink to an existing file reports size %llu, "
			  "so it was followed", (unsigned long long)at.size);
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Buffer handling. A short buffer must fail without writing a partial
	 * target, and must still report the length the caller needs. */
	CHECK(ntfs_lookup(v.root, "abs.lnk", &l) == 0);
	if (l) {
		n = 0;
		CHECK(ntfs_readlink(l, buf, sizeof(buf), &n) == -ERANGE);
		CHECK_MSG(n == strlen("/etc/hosts"),
			  "-ERANGE reported len_out %zu, so the caller cannot size a retry", n);
		/* buf == NULL is a size query. */
		n = 0;
		CHECK(ntfs_readlink(l, NULL, 0, &n) == 0);
		CHECK(n == strlen("/etc/hosts"));
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Refusals. An empty target has nowhere to point; the length limit is
	 * PATH_MAX, checked in ntfs_symlink() before anything is written. */
	CHECK(ntfs_symlink(v.root, "empty.lnk", "", &l) == -EINVAL);
	memset(big, 'a', PATH_MAX - 1);
	big[PATH_MAX - 1] = 0;			/* PATH_MAX-1 bytes: the longest accepted */
	CHECK(ntfs_symlink(v.root, "max.lnk", big, &l) == 0);
	if (l) { CHECK(ntfs_getattr(l, &at) == 0);
		 CHECK(at.size == PATH_MAX - 1); ntfs_inode_put(l); l = NULL; }
	big[PATH_MAX - 1] = 'a';
	big[PATH_MAX] = 0;			/* one over */
	CHECK(ntfs_symlink(v.root, "toolong.lnk", big, &l) == -ENAMETOOLONG);
	CHECK(ntfs_lookup(v.root, "toolong.lnk", &l) == -ENOENT);
	CHECK(ntfs_symlink(v.root, "abs.lnk", "/x", &l) == -EEXIST);

	/*
	 * A symlink has no addressable data stream. The target lives in the
	 * reparse point and getattr reports the target length as the size, so a
	 * write to $DATA used to succeed and leave the two disagreeing: 14 bytes
	 * in the stream, size still 10, and the bytes survived a remount and
	 * ntfsck without anything ever reading them again. POSIX has no
	 * write-to-a-symlink operation -- the layer above resolves the link and
	 * writes the target -- so every door to that stream is refused here.
	 * Not EISDIR and not EPERM: EINVAL, which is what Linux gives for an
	 * operation the file type does not have.
	 */
	CHECK(ntfs_lookup(v.root, "abs.lnk", &l) == 0);
	if (l) {
		uint64_t was;

		CHECK(ntfs_getattr(l, &at) == 0);
		was = at.size;
		CHECK_MSG(ntfs_write(l, "CLOBBER", 7, 0) == -EINVAL,
			  "writing a symlink's $DATA was not refused");
		CHECK_MSG(ntfs_read(l, buf, sizeof(buf), 0) == -EINVAL,
			  "reading a symlink's $DATA was not refused");
		CHECK_MSG(ntfs_truncate(l, 3) == -EINVAL,
			  "truncating a symlink was not refused");
		CHECK_MSG(ntfs_fallocate(l, 0, 4096, false) == -EINVAL,
			  "fallocate on a symlink was not refused");
		/* Refused means nothing moved: the size the caller sees and the
		 * target itself are both still what they were. */
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK_MSG(at.size == was, "a refused write still changed size to %llu",
			  (unsigned long long)at.size);
		check_target(l, "/etc/hosts", "abs.lnk after refused writes");
		/* Mode and times stay settable: only the size is refused. */
		at.mode = 0100600;
		CHECK_MSG(ntfs_setattr(l, &at, NTFS_SETATTR_MODE) == 0,
			  "setting the mode of a symlink was refused too");
		CHECK_MSG(ntfs_setattr(l, &at, NTFS_SETATTR_SIZE) == -EINVAL,
			  "NTFS_SETATTR_SIZE on a symlink was not refused");
		ntfs_inode_put(l);
		l = NULL;
	}

	vol_down(&v);
	fsck_clean("symlink creation");

	/*
	 * Remount. On the way in the target comes from ntfs_make_symlink()
	 * decoding the reparse attribute, a completely different path from the
	 * in-memory ni->target used above, so this is the one that proves the
	 * link is on the disk rather than in the cache.
	 */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_symlinks: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	{
		static const struct { const char *name, *target; } want[] = {
			{ "abs.lnk",     "/etc/hosts" },
			{ "rel.lnk",     "../sib/y.txt" },
			{ "dangle.lnk",  "no/such/file" },
			{ "utf8.lnk",    "caf\xc3\xa9/\xe2\x98\x83.txt" },
			{ "bs.lnk",      "a\\b" },
			{ "to-real.lnk", "real.txt" },
		};
		for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
			if (ntfs_lookup(v.root, want[i].name, &l) != 0) {
				CHECK_MSG(0, "%s is gone after a remount", want[i].name);
				continue;
			}
			CHECK(ntfs_getattr(l, &at) == 0);
			CHECK_MSG(at.type == NTFS_ITEM_SYMLINK,
				  "%s came back as item type %d, not a symlink",
				  want[i].name, at.type);
			CHECK_MSG(at.reparse_tag != 0, "%s has no reparse tag", want[i].name);
			check_target(l, want[i].target, want[i].name);
			ntfs_inode_put(l);
			l = NULL;
		}
		/* The long one too: its target does not fit the base MFT record
		 * comfortably, which is where a truncation would show. */
		if (ntfs_lookup(v.root, "max.lnk", &l) == 0) {
			n = 0;
			CHECK(ntfs_readlink(l, NULL, 0, &n) == 0);
			CHECK_MSG(n == PATH_MAX - 1,
				  "the %d-byte target came back as %zu bytes", PATH_MAX - 1, n);
			ntfs_inode_put(l);
			l = NULL;
		} else {
			CHECK_MSG(0, "max.lnk is gone after a remount");
		}
	}

	/* Deletion: a symlink goes away with ntfs_unlink, and taking it away
	 * must not take its target with it. */
	CHECK(ntfs_unlink(v.root, "to-real.lnk") == 0);
	CHECK(ntfs_lookup(v.root, "to-real.lnk", &l) == -ENOENT);
	CHECK_MSG(ntfs_lookup(v.root, "real.txt", &f) == 0,
		  "deleting a symlink deleted what it pointed at");
	if (f) { ntfs_inode_put(f); f = NULL; }
	CHECK(ntfs_unlink(v.root, "dangle.lnk") == 0);
	CHECK(ntfs_lookup(v.root, "dangle.lnk", &l) == -ENOENT);
	/* rmdir must not accept one. */
	CHECK(ntfs_rmdir(v.root, "abs.lnk") == -ENOTDIR);

	vol_down(&v);
	/* A reparse point has an entry in $Extend/$Reparse as well as its
	 * attribute; ntfs_delete removes it, and ntfsck is what notices if it
	 * did not. */
	fsck_clean("symlink deletion");
	unlink(scratch);
	printf("test_symlinks\n");
}

/* ---- 5. timestamps -------------------------------------------------- */

/*
 * The conversion on its own, at values that can be checked by hand. An error
 * in the epoch constant or the 100 ns scale moves every file on the volume,
 * and both mistakes produce dates that still look like dates.
 */
static void test_time_conversion(void)
{
	static const struct {
		const char *what;
		int64_t sec;
		int32_t nsec;
		uint64_t ntfs;		/* 100 ns ticks since 1601-01-01 */
	} known[] = {
		/* The NTFS epoch itself. */
		{ "1601-01-01T00:00:00Z", -11644473600LL, 0, 0ULL },
		/* One tick. This is the resolution: 100 ns. */
		{ "1601-01-01T00:00:00.0000001Z", -11644473600LL, 100, 1ULL },
		/* The Unix epoch, i.e. the offset constant itself:
		 * 134774 days * 86400 s * 10^7 ticks. */
		{ "1970-01-01T00:00:00Z", 0, 0, 116444736000000000ULL },
		{ "2001-01-01T00:00:00Z", 978307200LL, 0, 126227808000000000ULL },
		/* Past the signed 32-bit second, where a 32-bit time_t wraps. */
		{ "2038-01-19T03:14:08Z", 2147483648LL, 0, 137919572480000000ULL },
		/* A sub-second value that is an exact number of ticks. */
		/* (1234567890 + 11644473600) * 10^7 + 1234567 */
		{ "2009-02-13T23:31:30.1234567Z", 1234567890LL, 123456700, 128790414901234567ULL },
	};
	size_t i;

	CHECK_MSG(NTFS_TIME_OFFSET == 11644473600LL,
		  "the 1601->1970 offset is %lld s, expected 11644473600",
		  (long long)NTFS_TIME_OFFSET);

	for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		struct timespec64 ts = { .tv_sec = known[i].sec, .tv_nsec = known[i].nsec };
		u64 got = le64_to_cpu(utc2ntfs(ts));
		struct timespec64 back = ntfs2utc(cpu_to_le64(known[i].ntfs));

		CHECK_MSG(got == known[i].ntfs,
			  "utc2ntfs(%s) = %llu, expected %llu", known[i].what,
			  (unsigned long long)got, (unsigned long long)known[i].ntfs);
		/*
		 * ntfs2utc() must return a NORMALISED timespec at every value,
		 * including before 1970. div_s64_rem() truncates toward zero, so
		 * unfixed it hands back a negative tv_nsec for any pre-1970
		 * instant off a whole second (tick 1 gave tv_sec = -11644473599,
		 * tv_nsec = -999999900). The instant was arithmetically right
		 * either way, which is why nothing on disk was ever damaged, but
		 * these fields are copied into struct ntfs_timespec and handed
		 * over the public ABI, and POSIX requires 0 <= nsec < 10^9.
		 * core/ntfs/time.h borrows a second; see the PORT: comment there.
		 */
		CHECK_MSG(back.tv_nsec >= 0 && back.tv_nsec < 1000000000L,
			  "ntfs2utc(%s) is denormalised: tv_nsec = %ld, must be in [0, 10^9)",
			  known[i].what, (long)back.tv_nsec);
		CHECK_MSG(back.tv_sec == known[i].sec && back.tv_nsec == known[i].nsec,
			  "ntfs2utc(%s) = %lld.%09ld, expected %lld.%09d",
			  known[i].what, (long long)back.tv_sec, (long)back.tv_nsec,
			  (long long)known[i].sec, known[i].nsec);
	}

	/*
	 * Normalising must not cost the round trip: borrowing a second moves a
	 * whole second from tv_sec into tv_nsec, so utc2ntfs() has to give the
	 * original tick count back. Walk the ticks either side of both epochs,
	 * which is where the sign of the remainder changes.
	 */
	{
		static const uint64_t edges[] = {
			0, 1, 2, 9999999, 10000000, 10000001, 12345,
			116444735999999999ULL, 116444736000000000ULL,
			116444736000000001ULL,
		};
		size_t j;

		for (j = 0; j < sizeof(edges) / sizeof(edges[0]); j++) {
			struct timespec64 ts = ntfs2utc(cpu_to_le64(edges[j]));
			u64 back = le64_to_cpu(utc2ntfs(ts));

			CHECK_MSG(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L,
				  "ntfs2utc(tick %llu) denormalised: %lld.%09ld",
				  (unsigned long long)edges[j],
				  (long long)ts.tv_sec, (long)ts.tv_nsec);
			CHECK_MSG(back == edges[j],
				  "tick %llu round-tripped to %llu",
				  (unsigned long long)edges[j],
				  (unsigned long long)back);
		}
	}

	/* Sub-tick precision is lost, and must be lost downwards: rounding up
	 * would make an mtime move forward every time the inode is written. */
	{
		struct timespec64 ts = { .tv_sec = 1000000000LL, .tv_nsec = 99 };
		struct timespec64 back = ntfs2utc(utc2ntfs(ts));

		CHECK_MSG(back.tv_sec == 1000000000LL && back.tv_nsec == 0,
			  "99 ns truncated to %lld.%09ld, expected .000000000",
			  (long long)back.tv_sec, (long)back.tv_nsec);
	}
	printf("test_time_conversion\n");
}

/*
 * And the same values through a real mount: set, unmount, mount again, read
 * back. This is what catches a timestamp that is written to
 * $STANDARD_INFORMATION but never to the FILE_NAME copies, or the reverse.
 */
static void test_timestamps(void)
{
	static const struct { const char *name; int64_t sec; int32_t nsec; } cases[] = {
		{ "epoch1601", -11644473600LL, 0 },
		{ "unixzero",  0, 0 },
		{ "y2009",     1234567890LL, 123456700 },
		{ "y2038",     2147483648LL, 0 },
		{ "y2500",     16725225600LL, 0 },
		/* Not a whole tick: must come back truncated, not rounded. */
		{ "subtick",   1000000000LL, 999999999 },
	};
	struct vol v;
	ntfs_inode_t *f = NULL;
	struct ntfs_attr at;
	size_t i;
	int64_t before_m;

	if (copy_fixture("timestamps") != 0) {
		fprintf(stderr, "SKIP test_timestamps: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_timestamps: mount failed\n");
		unlink(scratch);
		return;
	}

	/* A fresh file gets a creation time, and it is not zero: a crtime of
	 * 1601 is what a missing conversion looks like. */
	CHECK(ntfs_create(v.root, "fresh.txt", 0100644, &f) == 0);
	if (f) {
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.crtime.sec > 1600000000LL,
			  "a new file's crtime is %lld, which is not a plausible now",
			  (long long)at.crtime.sec);
		CHECK(at.mtime.sec > 1600000000LL);

		/* A write must move mtime forward. An mtime that does not move
		 * makes every backup and sync tool skip the changed file. */
		{
			struct ntfs_attr set;
			memset(&set, 0, sizeof(set));
			set.mtime.sec = 1000; set.mtime.nsec = 0;
			CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_MTIME) == 0);
			CHECK(ntfs_getattr(f, &at) == 0);
			before_m = at.mtime.sec;
			CHECK(before_m == 1000);
			CHECK(ntfs_write(f, "x", 1, 0) == 1);
			CHECK(ntfs_getattr(f, &at) == 0);
			CHECK_MSG(at.mtime.sec > before_m,
				  "mtime did not move on write (still %lld)",
				  (long long)at.mtime.sec);
		}
		ntfs_inode_put(f);
		f = NULL;
	}

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct ntfs_attr set;

		if (ntfs_create(v.root, cases[i].name, 0100644, &f) != 0) {
			CHECK_MSG(0, "create %s failed", cases[i].name);
			continue;
		}
		memset(&set, 0, sizeof(set));
		set.atime.sec = set.mtime.sec = set.crtime.sec = cases[i].sec;
		set.atime.nsec = set.mtime.nsec = set.crtime.nsec = cases[i].nsec;
		CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_ATIME | NTFS_SETATTR_MTIME |
					    NTFS_SETATTR_CRTIME) == 0);
		/* Before any write-back: setattr must be visible immediately. */
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.mtime.sec == cases[i].sec && at.mtime.nsec == cases[i].nsec,
			  "%s: setattr mtime read back as %lld.%09d in memory",
			  cases[i].name, (long long)at.mtime.sec, at.mtime.nsec);
		ntfs_inode_put(f);
		f = NULL;
	}
	vol_down(&v);
	fsck_clean("timestamp writes");

	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_timestamps: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		/* What the disk can hold: whole 100 ns ticks. */
		int64_t want_sec = cases[i].sec;
		int32_t want_nsec = cases[i].nsec / 100 * 100;

		if (ntfs_lookup(v.root, cases[i].name, &f) != 0) {
			CHECK_MSG(0, "%s is gone after a remount", cases[i].name);
			continue;
		}
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.mtime.sec == want_sec && at.mtime.nsec == want_nsec,
			  "%s: mtime off disk is %lld.%09d, expected %lld.%09d",
			  cases[i].name, (long long)at.mtime.sec, at.mtime.nsec,
			  (long long)want_sec, want_nsec);
		CHECK_MSG(at.atime.sec == want_sec && at.atime.nsec == want_nsec,
			  "%s: atime off disk is %lld.%09d, expected %lld.%09d",
			  cases[i].name, (long long)at.atime.sec, at.atime.nsec,
			  (long long)want_sec, want_nsec);
		CHECK_MSG(at.crtime.sec == want_sec && at.crtime.nsec == want_nsec,
			  "%s: crtime off disk is %lld.%09d, expected %lld.%09d",
			  cases[i].name, (long long)at.crtime.sec, at.crtime.nsec,
			  (long long)want_sec, want_nsec);
		ntfs_inode_put(f);
		f = NULL;
	}
	vol_down(&v);
	unlink(scratch);
	printf("test_timestamps\n");
}

/* ---- 6. permissions ------------------------------------------------- */

/*
 * noowners. NTFS has no POSIX mode, so core/vfs/api.c mode_of() synthesises
 * one from the mount's fmask/dmask every time, and fill_attr() reports the
 * mount's uid/gid. Nothing stored on the volume feeds into it. If the masks
 * ever stop being applied every file on a USB stick becomes world-writable
 * with no visible change anywhere else.
 */
static void test_permissions(void)
{
	struct vol v;
	ntfs_inode_t *f = NULL, *d = NULL, *l = NULL;
	struct ntfs_attr at, set;

	if (copy_fixture("perms") != 0) {
		fprintf(stderr, "SKIP test_permissions: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_permissions: mount failed\n");
		unlink(scratch);
		return;
	}

	/* The mode passed to ntfs_create is NOT what comes back: 0777 & ~fmask
	 * is. Asking for 0600 and being given 0755 is the contract, odd as it
	 * looks, and the caller must not be able to talk the driver into
	 * storing per-file permissions it cannot honour later. */
	CHECK(ntfs_create(v.root, "f.bin", 0100600, &f) == 0);
	if (f) {
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG((at.mode & 07777) == (0777 & ~022),
			  "file mode is %o, expected %o (0777 & ~fmask 022)",
			  at.mode & 07777, 0777 & ~022);
		CHECK((at.mode & S_IFMT) == S_IFREG);
		CHECK_MSG(at.uid == 501 && at.gid == 20,
			  "uid/gid are %u/%u, expected the mount's 501/20", at.uid, at.gid);
	}
	CHECK(ntfs_mkdir(v.root, "d", 0700, &d) == 0);
	if (d) {
		CHECK(ntfs_getattr(d, &at) == 0);
		CHECK_MSG((at.mode & 07777) == (0777 & ~022),
			  "dir mode is %o, expected %o (0777 & ~dmask 022)",
			  at.mode & 07777, 0777 & ~022);
		CHECK((at.mode & S_IFMT) == S_IFDIR);
	}
	/* A symlink is always 0777: neither mask applies, because the mode of a
	 * symlink is never consulted for access on any POSIX system. */
	CHECK(ntfs_symlink(v.root, "s.lnk", "target", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK_MSG((at.mode & 07777) == 0777,
			  "symlink mode is %o, expected 0777", at.mode & 07777);
	}

	/*
	 * setattr(MODE) does not store a mode either. The only thing it can
	 * express on NTFS is the READONLY file attribute, taken from the write
	 * bits; the mode then comes back through the mask again, so clearing
	 * write on a volume mounted fmask=022 gives 0555, not 0400.
	 */
	if (f) {
		memset(&set, 0, sizeof(set));
		set.mode = 0100400;
		CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_MODE) == 0);
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG((at.mode & 07777) == 0555,
			  "after chmod 0400 the mode is %o, expected 0555", at.mode & 07777);
		CHECK_MSG(at.file_attributes & 0x1,
			  "chmod 0400 did not set the NTFS READONLY attribute (attrs %08x)",
			  at.file_attributes);

		memset(&set, 0, sizeof(set));
		set.mode = 0100644;
		CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_MODE) == 0);
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG((at.mode & 07777) == 0755,
			  "after chmod 0644 the mode is %o, expected 0755", at.mode & 07777);
		CHECK_MSG(!(at.file_attributes & 0x1),
			  "chmod 0644 did not clear READONLY (attrs %08x)", at.file_attributes);

		/* NTFS_SETATTR_UID/GID is accepted and ignored: noowners means
		 * there is nowhere to put it, and failing would break chown
		 * calls that macOS makes on its own. */
		memset(&set, 0, sizeof(set));
		set.uid = 4242; set.gid = 4242;
		CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_UID | NTFS_SETATTR_GID) == 0);
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.uid == 501 && at.gid == 20,
			  "a chown was honoured: uid/gid are now %u/%u", at.uid, at.gid);
	}
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (l) { ntfs_inode_put(l); l = NULL; }
	vol_down(&v);

	/*
	 * Remount with different masks. The same files must report different
	 * modes: this is what proves the mode comes from the mount options and
	 * not from something that was stored at create time.
	 */
	if (vol_up(&v, 077, 027, 0, 0)) {
		fprintf(stderr, "FAIL test_permissions: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	if (ntfs_lookup(v.root, "f.bin", &f) == 0) {
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG((at.mode & 07777) == (0777 & ~077),
			  "under fmask 077 the file mode is %o, expected %o",
			  at.mode & 07777, 0777 & ~077);
		CHECK_MSG(at.uid == 0 && at.gid == 0,
			  "under uid=0,gid=0 the file reports %u/%u", at.uid, at.gid);
		ntfs_inode_put(f);
		f = NULL;
	} else {
		CHECK_MSG(0, "f.bin is gone after a remount");
	}
	if (ntfs_lookup(v.root, "d", &d) == 0) {
		CHECK(ntfs_getattr(d, &at) == 0);
		CHECK_MSG((at.mode & 07777) == (0777 & ~027),
			  "under dmask 027 the dir mode is %o, expected %o",
			  at.mode & 07777, 0777 & ~027);
		ntfs_inode_put(d);
		d = NULL;
	} else {
		CHECK_MSG(0, "d is gone after a remount");
	}
	if (ntfs_lookup(v.root, "s.lnk", &l) == 0) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK_MSG((at.mode & 07777) == 0777,
			  "a symlink picked up fmask: mode is %o", at.mode & 07777);
		ntfs_inode_put(l);
		l = NULL;
	}
	vol_down(&v);
	unlink(scratch);
	printf("test_permissions\n");
}

int main(void)
{
	test_hard_links();
	test_hard_link_refusals();
	test_many_links();
	test_symlinks();
	test_symlink_tag_choice();
	test_time_conversion();
	test_timestamps();
	test_permissions();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
