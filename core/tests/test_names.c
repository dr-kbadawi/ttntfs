// SPDX-License-Identifier: GPL-2.0
/*
 * test_names.c - the filename and xattr policy from docs/PORTING.md section 6.
 *
 * Three decisions were recorded there and none of them had a test:
 *
 *   1. "Filenames illegal on Windows are rejected with EINVAL."
 *      A file the Mac writes that Windows cannot open is a support ticket that
 *      arrives long after the write, on someone else's machine, with no way to
 *      rename the file from the side that can still see it. The check is the
 *      only thing standing between a Finder "New Folder" and that ticket, and
 *      it is one `if` in ntfs_vfs_validate_uname() that nothing exercised.
 *
 *   2. "Names are case-insensitive, case-preserving."
 *      If lookup stops folding case, creating FOO.TXT next to an existing
 *      Foo.txt makes a second directory entry instead of colliding. Windows
 *      then shows one of the two and the other is unreachable: silent data
 *      loss, and the kind that a read-back test does not notice because both
 *      entries read back fine. Folding is a table lookup in $UpCase, not the
 *      ASCII arithmetic, so the non-ASCII pairs below are the ones that would
 *      break first if anyone "simplified" it.
 *
 *   3. "macOS xattrs are stored as alternate data streams. No ._ files."
 *      The whole point is that a Mac-written volume carries its metadata where
 *      Windows and Linux ignore it harmlessly, rather than littering every
 *      directory with AppleDouble sidecars that users then copy around.
 *
 * Everything asserted here was first observed against the real code; where the
 * port refuses something or diverges from the note in PORTING.md, the comment
 * says so rather than the assertion being softened.
 *
 * Each mutating test finishes with ntfsprogs-plus `ntfsck -n`. Names live in
 * the directory index B-tree, where a bad insert corrupts a node that still
 * reads back correctly through the same code that wrote it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdbool.h>

#include "ntfscore.h"
#include "ntfsport/bdev.h"

/*
 * ntfs_setxattr()'s flags now come from ntfscore.h (core/include/
 * ntfs_xattr_flags.h) as NTFS_XATTR_CREATE / NTFS_XATTR_REPLACE, and
 * platform/include/linux/xattr.h derives its XATTR_CREATE / XATTR_REPLACE from
 * the same two macros, so there is one definition rather than the several that
 * used to be copied around. This file tests that from the outside, the way a
 * real consumer sees it: it links against the built core, so the numbers below
 * are whatever the core was actually compiled with.
 *
 * Do NOT include <sys/xattr.h> here. Darwin spells the same two ideas
 * XATTR_CREATE 2 and XATTR_REPLACE 4, and letting those names into this file is
 * how the confusion starts. The values are restated as literals instead.
 */
_Static_assert(NTFS_XATTR_CREATE == 1,
	       "ABI break: NTFS_XATTR_CREATE is 1, the Linux value the core tests");
_Static_assert(NTFS_XATTR_REPLACE == 2,
	       "ABI break: NTFS_XATTR_REPLACE is 2, the Linux value the core tests");

/* Darwin's numbers for the same two names, written out so the two can be
 * compared without dragging the conflicting macros in. If these ever coincide
 * the warnings elsewhere are stale and need rewriting -- but they must not be
 * made to coincide by moving the core's values, which would silently change
 * what every existing caller's flags mean. */
#define DARWIN_XATTR_CREATE	2
#define DARWIN_XATTR_REPLACE	4
_Static_assert(NTFS_XATTR_CREATE != DARWIN_XATTR_CREATE &&
	       NTFS_XATTR_REPLACE != DARWIN_XATTR_REPLACE,
	       "the core's xattr flags are deliberately not Darwin's; see ntfs_xattr_flags.h");
/* The trap this whole finding is about: Darwin's CREATE is the core's REPLACE,
 * so the mistake is a wrong answer rather than an error. */
_Static_assert(DARWIN_XATTR_CREATE == NTFS_XATTR_REPLACE,
	       "Darwin's XATTR_CREATE is the core's REPLACE -- the reason this matters");

static int failures, checks;

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

/* Like CHECK but prints what was actually returned: for a policy test the
 * difference between -EINVAL and -ENOENT is the whole finding. */
#define CHECK_RC(got, want, what) do {					\
	int g_ = (got), w_ = (want);					\
	checks++;							\
	if (g_ != w_) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s: got %d, expected %d\n",	\
			__FILE__, __LINE__, (what), g_, w_);		\
	}								\
} while (0)

static const char *images_dir(void)
{
	const char *d = getenv("NTFS_IMAGES");
	return d && *d ? d : "tools/images";
}

static const char *tools_dir(void)
{
	const char *d = getenv("NTFS_TOOLS");
	return d && *d ? d : "tools";
}

/* ---- a scratch copy of a fixture ------------------------------------- */

static char scratch[1024];

static int copy_fixture(const char *name, const char *tag)
{
	char src[1024];
	int in, out;
	char buf[1 << 16];
	ssize_t n;

	snprintf(src, sizeof(src), "%s/%s", images_dir(), name);
	snprintf(scratch, sizeof(scratch), "/tmp/ttntfs-names-%d-%s.img",
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

static struct ntfs_bdev *dev;

static int mount_scratch(uint32_t flags, ntfs_volume_t **vol, ntfs_inode_t **root)
{
	struct ntfs_mount_options o = { .flags = flags, .uid = 0, .gid = 0,
					.fmask = 022, .dmask = 022 };
	int err;

	dev = ntfs_bdev_open_path(scratch, false);
	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &o, vol);
	if (err)
		return err;
	return ntfs_volume_root(*vol, root);
}

/* ---- independent readers -------------------------------------------- */

static int run_quiet(const char *cmd)
{
	char full[2200];
	int rc;

	snprintf(full, sizeof(full), "%s >/dev/null 2>&1", cmd);
	rc = system(full);
	if (rc == -1)
		return -1;
	return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static bool tool_exists(const char *path)
{
	return access(path, X_OK) == 0;
}

/*
 * ntfsprogs-plus fsck: the only checker here that walks the index B-tree.
 * Always -n. Repairing the image under test would measure the repair.
 */
static void structural_check(const char *what)
{
	char bin[1200], cmd[2400];
	int rc;

	snprintf(bin, sizeof(bin), "%s/.local-plus/sbin/ntfsck", tools_dir());
	if (!tool_exists(bin)) {
		fprintf(stderr, "SKIP structural check after %s: %s not built "
			"(tools/build-ntfsprogs-plus.sh)\n", what, bin);
		return;
	}
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s'", bin, scratch);
	rc = run_quiet(cmd);
	checks++;
	if (rc != 0) {
		failures++;
		fprintf(stderr, "FAIL: ntfsck -n after %s: exit %d\n", what, rc);
	}
}

/* Does an independent reader see @name in the image's root directory?
 * ntfsls is the oracle: a name that only our own readdir can see is a name
 * Windows will not find either. */
static bool ntfsls_root_has(const char *name)
{
	char bin[1200], cmd[2400], line[1024];
	FILE *p;
	bool found = false;

	snprintf(bin, sizeof(bin), "%s/.local/bin/ntfsls", tools_dir());
	if (!tool_exists(bin))
		return true;		/* no oracle: do not manufacture a failure */
	snprintf(cmd, sizeof(cmd), "'%s' '%s' 2>/dev/null", bin, scratch);
	p = popen(cmd, "r");
	if (!p)
		return true;
	while (fgets(line, sizeof(line), p)) {
		line[strcspn(line, "\n")] = 0;
		if (!strcmp(line, name))
			found = true;
	}
	pclose(p);
	return found;
}

/* Read a named $DATA stream with ntfscat, which knows nothing about xattrs.
 * Returns the byte count read into @buf, or -1. */
static int ntfscat_stream(const char *file, const char *stream, char *buf, size_t size)
{
	char bin[1200], cmd[2600];
	FILE *p;
	size_t n;

	snprintf(bin, sizeof(bin), "%s/.local/bin/ntfscat", tools_dir());
	if (!tool_exists(bin))
		return -1;
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' '%s' '%s' 2>/dev/null",
		 bin, stream, scratch, file);
	p = popen(cmd, "r");
	if (!p)
		return -1;
	n = fread(buf, 1, size, p);
	pclose(p);
	return (int)n;
}

/* ---- readdir helpers ------------------------------------------------- */

struct namelist {
	char names[256][1100];
	int n;
};

static int collect_cb(const struct ntfs_dirent *e, void *ctx)
{
	struct namelist *l = ctx;

	if (l->n < 256 && e->name_len < sizeof(l->names[0])) {
		memcpy(l->names[l->n], e->name, e->name_len);
		l->names[l->n][e->name_len] = 0;
		l->n++;
	}
	return 0;
}

static void list_dir(ntfs_inode_t *d, struct namelist *l)
{
	uint64_t cookie = 0;
	bool eof = false;

	l->n = 0;
	while (!eof && ntfs_readdir(d, &cookie, false, collect_cb, l, &eof) == 0)
		;
}

static bool listed(const struct namelist *l, const char *name)
{
	for (int i = 0; i < l->n; i++)
		if (!strcmp(l->names[i], name))
			return true;
	return false;
}

static int listed_count(const struct namelist *l, const char *name)
{
	int c = 0;

	for (int i = 0; i < l->n; i++)
		if (!strcmp(l->names[i], name))
			c++;
	return c;
}

/* ---- 1. names Windows refuses --------------------------------------- */

/*
 * The list PORTING.md section 6 records is `: ? * < > | "` plus a trailing dot
 * or space. The implementation (ntfs_check_bad_char) rejects a superset, and
 * that superset is deliberate rather than accidental: it also refuses `/` and
 * `\` (both path separators on Windows) and every character below U+0020
 * (Windows refuses control characters in a name outright). All of it is
 * asserted, because a later "clean-up" that narrows the set back to the six
 * characters in the table would still pass a test that only checked those six.
 */
static const char *const illegal_names[] = {
	"co:lon.txt",
	"qu?mark.txt",
	"star*.txt",
	"less<than.txt",
	"more>than.txt",
	"pipe|bar.txt",
	"dquote\".txt",
	"back\\slash.txt",		/* not in the table; rejected all the same */
	"tab\there.txt",		/* U+0009, below U+0020 */
	"bell\x07.txt",			/* U+0007 */
	"trailing-dot.",
	"trailing-space ",
};

/*
 * MS-DOS device names. Windows refuses them with or without an extension,
 * because CreateFile still resolves CON to the console. The port implements
 * the whole set, in any case, and so does the check below.
 */
static const char *const reserved_names[] = {
	"CON", "con", "CoN", "CON.txt", "con.txt.bak",
	"PRN", "prn.log", "AUX", "aux.dat", "NUL", "nul.tmp",
	"COM1", "COM9", "com1.txt", "LPT1", "LPT9", "lpt5.doc",
};

/*
 * Names that look reserved and are not. Windows accepts every one of these,
 * so rejecting them would be the port inventing a restriction of its own --
 * which is worse than the bug it is guarding against, because the user cannot
 * turn it off by moving the file to Windows.
 */
static const char *const near_miss_names[] = {
	"COM0", "COM10", "COMA", "CONSOLE", "CONX", "AUXILIARY", "NULL",
	"LPT0", "PRNT", "CO.txt",
};

/* Accepted under either policy: unremarkable names that must not be caught by
 * an over-eager check. A leading space and a leading dot are both legal. */
static const char *const legal_names[] = {
	"plain.txt",
	"with spaces in name.txt",
	" leading-space.txt",
	".hidden-dotfile",
	"..two-leading-dots",
	"dots.in.the.name.tar.gz",
	"semi;colon,comma=equals+plus.txt",
	"[brackets]{braces}(parens).txt",
	"~tilde#hash%percent&amp@at!bang.txt",
	"$dollar.txt",
	"'single'quotes'.txt",
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static void test_windows_illegal_rejected(void)
{
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *d, *ni;
	size_t i;
	char msg[256];

	if (copy_fixture("names.img", "strict") != 0) {
		fprintf(stderr, "SKIP test_windows_illegal_rejected: no fixture\n");
		return;
	}
	if (mount_scratch(0, &vol, &root)) {
		fprintf(stderr, "FAIL: could not mount for strict-policy test\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}

	CHECK(ntfs_mkdir(root, "strict", 040755, &d) == 0);

	for (i = 0; i < ARRAY_LEN(illegal_names); i++) {
		snprintf(msg, sizeof(msg), "validate '%s'", illegal_names[i]);
		CHECK_RC(ntfs_validate_name(vol, illegal_names[i]), -EINVAL, msg);
		snprintf(msg, sizeof(msg), "create '%s'", illegal_names[i]);
		CHECK_RC(ntfs_create(d, illegal_names[i], 0100644, NULL), -EINVAL, msg);
		snprintf(msg, sizeof(msg), "mkdir '%s'", illegal_names[i]);
		CHECK_RC(ntfs_mkdir(d, illegal_names[i], 040755, NULL), -EINVAL, msg);
	}

	for (i = 0; i < ARRAY_LEN(reserved_names); i++) {
		snprintf(msg, sizeof(msg), "validate reserved '%s'", reserved_names[i]);
		CHECK_RC(ntfs_validate_name(vol, reserved_names[i]), -EINVAL, msg);
		snprintf(msg, sizeof(msg), "create reserved '%s'", reserved_names[i]);
		CHECK_RC(ntfs_create(d, reserved_names[i], 0100644, NULL), -EINVAL, msg);
	}

	for (i = 0; i < ARRAY_LEN(near_miss_names); i++) {
		snprintf(msg, sizeof(msg), "validate near-miss '%s'", near_miss_names[i]);
		CHECK_RC(ntfs_validate_name(vol, near_miss_names[i]), 0, msg);
		snprintf(msg, sizeof(msg), "create near-miss '%s'", near_miss_names[i]);
		CHECK_RC(ntfs_create(d, near_miss_names[i], 0100644, NULL), 0, msg);
	}

	for (i = 0; i < ARRAY_LEN(legal_names); i++) {
		snprintf(msg, sizeof(msg), "validate legal '%s'", legal_names[i]);
		CHECK_RC(ntfs_validate_name(vol, legal_names[i]), 0, msg);
		snprintf(msg, sizeof(msg), "create legal '%s'", legal_names[i]);
		CHECK_RC(ntfs_create(d, legal_names[i], 0100644, NULL), 0, msg);
	}

	/*
	 * Every write path that takes a name must apply the policy, not just
	 * create: a rename is how a legal name becomes an illegal one.
	 */
	CHECK(ntfs_create(d, "renameme.txt", 0100644, &ni) == 0);
	if (ni)
		ntfs_inode_put(ni);
	CHECK_RC(ntfs_rename(d, "renameme.txt", d, "re:named.txt"), -EINVAL,
		 "rename to an illegal name");
	CHECK_RC(ntfs_rename(d, "renameme.txt", d, "NUL"), -EINVAL,
		 "rename to a reserved name");
	CHECK_RC(ntfs_link(NULL, d, "li:nk.txt"), -EINVAL, "link with a null inode");
	CHECK(ntfs_lookup(d, "renameme.txt", &ni) == 0);
	if (ni) {
		CHECK_RC(ntfs_link(ni, d, "li:nk.txt"), -EINVAL, "hard link to an illegal name");
		CHECK_RC(ntfs_link(ni, d, "LPT3"), -EINVAL, "hard link to a reserved name");
		ntfs_inode_put(ni);
	}
	CHECK_RC(ntfs_symlink(d, "sym:link", "renameme.txt", NULL), -EINVAL,
		 "symlink with an illegal name");

	/* The refused names left nothing behind. */
	{
		struct namelist l;

		list_dir(d, &l);
		for (i = 0; i < ARRAY_LEN(illegal_names); i++)
			CHECK(!listed(&l, illegal_names[i]));
		for (i = 0; i < ARRAY_LEN(reserved_names); i++)
			CHECK(!listed(&l, reserved_names[i]));
		CHECK(!listed(&l, "sym:link"));
		CHECK(!listed(&l, "li:nk.txt"));
	}

	ntfs_inode_put(d);
	ntfs_inode_put(root);
	CHECK(ntfs_unmount(vol) == 0);
	structural_check("test_windows_illegal_rejected");
	unlink(scratch);
	printf("test_windows_illegal_rejected\n");
}

/* ---- 2. ... unless the mount says otherwise -------------------------- */

static void test_windows_illegal_allowed(void)
{
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *ni;
	struct namelist l;
	size_t i;
	char msg[256];

	if (copy_fixture("names.img", "allow") != 0) {
		fprintf(stderr, "SKIP test_windows_illegal_allowed: no fixture\n");
		return;
	}
	/* -o allowillegal in the FSKit layer (Options.swift also spells it
	 * windows_names_off); NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL here, which
	 * super_glue.c turns into "do not pass windows_names" to the core. */
	if (mount_scratch(NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL, &vol, &root)) {
		fprintf(stderr, "FAIL: could not mount with allowillegal\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}

	for (i = 0; i < ARRAY_LEN(illegal_names); i++) {
		snprintf(msg, sizeof(msg), "allowillegal validate '%s'", illegal_names[i]);
		CHECK_RC(ntfs_validate_name(vol, illegal_names[i]), 0, msg);
		snprintf(msg, sizeof(msg), "allowillegal create '%s'", illegal_names[i]);
		CHECK_RC(ntfs_create(root, illegal_names[i], 0100644, NULL), 0, msg);
	}
	for (i = 0; i < ARRAY_LEN(reserved_names); i++) {
		snprintf(msg, sizeof(msg), "allowillegal validate reserved '%s'",
			 reserved_names[i]);
		CHECK_RC(ntfs_validate_name(vol, reserved_names[i]), 0, msg);
	}
	/* Only the case-distinct spellings; "con" after "CON" is an
	 * EEXIST collision, which is decision 2's business, not this one. */
	CHECK_RC(ntfs_create(root, "CON", 0100644, NULL), 0, "allowillegal create CON");
	CHECK_RC(ntfs_create(root, "COM1", 0100644, NULL), 0, "allowillegal create COM1");
	CHECK_RC(ntfs_create(root, "NUL.tmp", 0100644, NULL), 0, "allowillegal create NUL.tmp");

	/*
	 * `/` is not part of the Windows name policy and the mount option does
	 * not reach it: the public ABI takes one path component, and
	 * check_component() in core/vfs/api.c refuses a separator, "." and ".."
	 * before any volume is consulted. Allowing a `/` through would let a
	 * caller write a name no path can ever address again.
	 */
	CHECK_RC(ntfs_validate_name(vol, "slash/inside.txt"), -EINVAL,
		 "allowillegal still refuses a path separator");
	CHECK_RC(ntfs_create(root, "slash/inside.txt", 0100644, NULL), -EINVAL,
		 "allowillegal still refuses a path separator on create");
	CHECK_RC(ntfs_validate_name(vol, "."), -EINVAL, "'.' is never a name");
	CHECK_RC(ntfs_validate_name(vol, ".."), -EINVAL, "'..' is never a name");
	CHECK_RC(ntfs_create(root, "", 0100644, NULL), -EINVAL, "empty name");

	/* The names really landed, and our own readdir is not the only witness. */
	list_dir(root, &l);
	for (i = 0; i < ARRAY_LEN(illegal_names); i++) {
		snprintf(msg, sizeof(msg), "'%s' in readdir", illegal_names[i]);
		checks++;
		if (!listed(&l, illegal_names[i])) {
			failures++;
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);
		}
	}
	CHECK(!listed(&l, "slash/inside.txt"));

	ntfs_inode_put(root);
	CHECK(ntfs_unmount(vol) == 0);

	CHECK(ntfsls_root_has("co:lon.txt"));
	CHECK(ntfsls_root_has("trailing-space "));
	CHECK(ntfsls_root_has("CON"));
	structural_check("test_windows_illegal_allowed");

	/*
	 * A strict mount must still read and delete what a permissive one
	 * wrote. The policy is a rule about what this driver creates; applying
	 * it to lookup would strand files that WSL, ntfs-3g or an older mount
	 * already put on the volume, with no way to rename them from macOS.
	 */
	if (mount_scratch(0, &vol, &root) == 0) {
		CHECK_RC(ntfs_lookup(root, "co:lon.txt", &ni), 0,
			 "strict mount can still look up an illegal name");
		if (ni)
			ntfs_inode_put(ni);
		CHECK_RC(ntfs_lookup(root, "CON", &ni), 0,
			 "strict mount can still look up a reserved name");
		if (ni)
			ntfs_inode_put(ni);
		CHECK_RC(ntfs_unlink(root, "co:lon.txt"), 0,
			 "strict mount can still delete an illegal name");
		/* But renaming it to another illegal name is still refused. */
		CHECK_RC(ntfs_rename(root, "CON", root, "PRN"), -EINVAL,
			 "strict mount refuses an illegal rename target");
		/* Renaming it to a legal one is how the user gets out. */
		CHECK_RC(ntfs_rename(root, "CON", root, "console.txt"), 0,
			 "strict mount can rename an illegal name to a legal one");
		ntfs_inode_put(root);
		CHECK(ntfs_unmount(vol) == 0);
		structural_check("strict remount over illegal names");
	} else {
		fprintf(stderr, "FAIL: could not remount strict over illegal names\n");
		failures++; checks++;
	}
	unlink(scratch);
	printf("test_windows_illegal_allowed\n");
}

/* ---- 3. case-insensitive, case-preserving --------------------------- */

/*
 * Pairs that the $UpCase table written by mkntfs folds together. The non-ASCII
 * ones are the point: NTFS folds through a 65536-entry table, not by adding
 * 0x20, and U+00FF -> U+0178 moves backwards by 0x87 and out of the Latin-1
 * block entirely. Anything that reimplements folding as arithmetic passes the
 * ASCII row and fails that one.
 */
static const struct case_pair {
	const char *lower, *upper, *what;
} folding_pairs[] = {
	{ "Foo.txt",              "FOO.TXT",              "ASCII" },
	{ "\xc3\xbf-fold.txt",    "\xc5\xb8-fold.txt",    "U+00FF y-diaeresis -> U+0178, delta -0x87" },
	{ "\xc4\x81-fold.txt",    "\xc4\x80-fold.txt",    "U+0101 a-macron -> U+0100, delta -1" },
	{ "\xc7\x86-fold.txt",    "\xc7\x84-fold.txt",    "U+01C6 dz -> U+01C4 DZ, delta -2" },
	{ "\xe2\x85\xb0-fold.txt","\xe2\x85\xa0-fold.txt","U+2170 small roman i -> U+2160, delta -0x10" },
	{ "\xce\xb1\xce\xb2-f.txt","\xce\x91\xce\x92-f.txt","Greek alpha beta" },
	{ "\xd0\xb4\xd0\xbe-f.txt","\xd0\x94\xd0\x9e-f.txt","Cyrillic de o" },
	{ "\xd5\xa1-fold.txt",    "\xd4\xb1-fold.txt",    "Armenian ayb U+0561 -> U+0531" },
	{ "caf\xc3\xa9-f.txt",    "CAF\xc3\x89-F.TXT",    "Latin-1 e-acute" },
	{ "\xef\xbd\x81\xef\xbd\x82-f.txt", "\xef\xbc\xa1\xef\xbc\xa2-F.TXT", "fullwidth a b" },
};

/*
 * Pairs that Unicode case-folds and NTFS does not. $UpCase is a 1:1 map of
 * 16-bit code units, so a fold that changes length (sharp s -> SS) or that
 * Unicode marks as locale- or context-dependent has no entry, and Windows
 * treats the two names as distinct. That is the behaviour to preserve: the
 * volume has to agree with the machine that formatted it, not with ICU.
 * Recorded here so that "improving" the folding shows up as a failure.
 */
static const struct case_pair non_folding_pairs[] = {
	{ "stra\xc3\x9f""e-nf.txt", "STRASSE-NF.TXT",  "German sharp s does not expand to SS" },
	{ "\xc4\xb1stanbul-nf.txt", "Istanbul-nf.txt", "Turkish dotless i U+0131 is not I" },
	{ "\xcf\x82-nf.txt",        "\xce\xa3-nf.txt", "Greek final sigma U+03C2 is not Sigma" },
	{ "\xe1\xba\x9b-nf.txt",    "\xe1\xb9\xa0-nf.txt", "U+1E9B long s with dot is not U+1E60" },
};

static void test_case_insensitive_preserving(void)
{
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *d, *a, *b;
	struct namelist l;
	size_t i;
	char msg[256];

	if (copy_fixture("names.img", "case") != 0) {
		fprintf(stderr, "SKIP test_case_insensitive_preserving: no fixture\n");
		return;
	}
	if (mount_scratch(0, &vol, &root)) {
		fprintf(stderr, "FAIL: could not mount for the case test\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	CHECK(ntfs_mkdir(root, "casedir", 040755, &d) == 0);

	for (i = 0; i < ARRAY_LEN(folding_pairs); i++) {
		const struct case_pair *p = &folding_pairs[i];

		a = b = NULL;
		snprintf(msg, sizeof(msg), "create lower (%s)", p->what);
		CHECK_RC(ntfs_create(d, p->lower, 0100644, &a), 0, msg);

		/* Insensitive: the other case finds the same inode. */
		snprintf(msg, sizeof(msg), "lookup upper (%s)", p->what);
		CHECK_RC(ntfs_lookup(d, p->upper, &b), 0, msg);
		if (a && b) {
			snprintf(msg, sizeof(msg), "same inode (%s)", p->what);
			CHECK_RC((int)(ntfs_inode_number(b) - ntfs_inode_number(a)), 0, msg);
		}

		/*
		 * Collides rather than making a second entry. Two entries here
		 * is the silent-data-loss case: both read back fine from this
		 * driver and Windows shows only one of them.
		 */
		snprintf(msg, sizeof(msg), "create upper collides (%s)", p->what);
		CHECK_RC(ntfs_create(d, p->upper, 0100644, NULL), -EEXIST, msg);
		snprintf(msg, sizeof(msg), "mkdir upper collides (%s)", p->what);
		CHECK_RC(ntfs_mkdir(d, p->upper, 040755, NULL), -EEXIST, msg);

		if (a) ntfs_inode_put(a);
		if (b) ntfs_inode_put(b);
	}

	for (i = 0; i < ARRAY_LEN(non_folding_pairs); i++) {
		const struct case_pair *p = &non_folding_pairs[i];

		b = NULL;
		snprintf(msg, sizeof(msg), "create (%s)", p->what);
		CHECK_RC(ntfs_create(d, p->lower, 0100644, NULL), 0, msg);
		snprintf(msg, sizeof(msg), "lookup finds nothing (%s)", p->what);
		CHECK_RC(ntfs_lookup(d, p->upper, &b), -ENOENT, msg);
		if (b) ntfs_inode_put(b);
		snprintf(msg, sizeof(msg), "second create succeeds (%s)", p->what);
		CHECK_RC(ntfs_create(d, p->upper, 0100644, NULL), 0, msg);
	}

	/* Case-preserving: what was created is what is listed, exactly once. */
	list_dir(d, &l);
	for (i = 0; i < ARRAY_LEN(folding_pairs); i++) {
		const struct case_pair *p = &folding_pairs[i];

		checks++;
		if (!listed(&l, p->lower)) {
			failures++;
			fprintf(stderr, "FAIL %s:%d: '%s' not listed in its original case (%s)\n",
				__FILE__, __LINE__, p->lower, p->what);
		}
		checks++;
		if (listed(&l, p->upper)) {
			failures++;
			fprintf(stderr, "FAIL %s:%d: '%s' listed as a second entry (%s)\n",
				__FILE__, __LINE__, p->upper, p->what);
		}
	}

	/*
	 * A case-only rename is the one way the stored case is meant to change.
	 * The port does it as two renames through a ".ntfstmp" name, because the
	 * index cannot hold both spellings at once; that dance must leave one
	 * entry with the new case and no temporary behind.
	 */
	CHECK(ntfs_create(d, "CaseRename.txt", 0100644, NULL) == 0);
	CHECK_RC(ntfs_rename(d, "CaseRename.txt", d, "CASERENAME.TXT"), 0,
		 "case-only rename");
	list_dir(d, &l);
	CHECK(listed(&l, "CASERENAME.TXT"));
	CHECK(!listed(&l, "CaseRename.txt"));
	CHECK_RC(listed_count(&l, "CASERENAME.TXT"), 1, "exactly one entry after a case rename");
	for (i = 0; i < (size_t)l.n; i++)
		CHECK(!strstr(l.names[i], ".ntfstmp"));

	/* Renaming a name onto itself is a no-op, not a collision. */
	CHECK_RC(ntfs_rename(d, "CASERENAME.TXT", d, "CASERENAME.TXT"), 0,
		 "rename to the identical name");

	ntfs_inode_put(d);
	ntfs_inode_put(root);
	CHECK(ntfs_unmount(vol) == 0);
	structural_check("test_case_insensitive_preserving");
	unlink(scratch);
	printf("test_case_insensitive_preserving\n");
}

/* ---- 4. length, surrogates, and the absence of normalisation --------- */

static void test_long_and_unicode_names(void)
{
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *d, *ni;
	struct namelist l;
	char name[1200];
	size_t p;
	int i;

	if (copy_fixture("names.img", "len") != 0) {
		fprintf(stderr, "SKIP test_long_and_unicode_names: no fixture\n");
		return;
	}
	if (mount_scratch(0, &vol, &root)) {
		fprintf(stderr, "FAIL: could not mount for the length test\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	CHECK(ntfs_mkdir(root, "lendir", 040755, &d) == 0);

	/*
	 * The NTFS limit is 255 UTF-16 code units, not 255 bytes and not 255
	 * characters. All three tests below are the same limit measured three
	 * ways, and only the unit count is right.
	 */
	memset(name, 'a', 255);
	name[255] = 0;
	CHECK_RC(ntfs_create(d, name, 0100644, NULL), 0, "255 ASCII units");
	memset(name, 'b', 256);
	name[256] = 0;
	CHECK_RC(ntfs_create(d, name, 0100644, NULL), -ENAMETOOLONG, "256 ASCII units");

	/* 255 CJK characters = 255 units = 765 UTF-8 bytes, well past any
	 * byte-based limit anyone might substitute for the real one. */
	p = 0;
	for (i = 0; i < 255; i++) {
		unsigned cp = 0x4E00 + (unsigned)i;
		name[p++] = (char)(0xE0 | (cp >> 12));
		name[p++] = (char)(0x80 | ((cp >> 6) & 63));
		name[p++] = (char)(0x80 | (cp & 63));
	}
	name[p] = 0;
	CHECK_RC(ntfs_create(d, name, 0100644, NULL), 0, "255 CJK units (765 bytes)");

	/*
	 * Surrogate pairs. U+1F600 is one character, four UTF-8 bytes and TWO
	 * UTF-16 units, so 127 of them fit and 128 do not. A limit that counted
	 * characters would accept 128 and write a 256-unit name that Windows
	 * cannot represent in a $FILE_NAME attribute.
	 */
	p = 0;
	for (i = 0; i < 127; i++) { memcpy(name + p, "\xf0\x9f\x98\x80", 4); p += 4; }
	name[p] = 0;
	CHECK_RC(ntfs_create(d, name, 0100644, NULL), 0, "127 emoji = 254 units");
	CHECK_RC(ntfs_lookup(d, name, &ni), 0, "lookup a 254-unit surrogate name");
	if (ni) ntfs_inode_put(ni);
	p = 0;
	for (i = 0; i < 128; i++) { memcpy(name + p, "\xf0\x9f\x98\x81", 4); p += 4; }
	name[p] = 0;
	CHECK_RC(ntfs_create(d, name, 0100644, NULL), -ENAMETOOLONG,
		 "128 emoji = 256 units");

	/* A short surrogate-pair name round-trips byte for byte. */
	CHECK_RC(ntfs_create(d, "emoji-\xf0\x9f\x8e\x89.txt", 0100644, NULL), 0,
		 "surrogate pair in a short name");
	CHECK_RC(ntfs_lookup(d, "emoji-\xf0\x9f\x8e\x89.txt", &ni), 0,
		 "lookup a surrogate-pair name");
	if (ni) ntfs_inode_put(ni);

	/*
	 * NFC and NFD. The port stores names verbatim; it does not normalise in
	 * either direction. "cafe" + U+0301 and "caf" + U+00E9 are therefore two
	 * different names that coexist, which is what NTFS does and what a
	 * Windows or Linux reader of the same volume will see. HFS+ normalised
	 * to a variant of NFD and that behaviour is deliberately not reproduced:
	 * a name that changed shape on the way to disk is a name that no longer
	 * matches what the application asked for.
	 */
	CHECK_RC(ntfs_create(d, "caf\xc3\xa9-norm.txt", 0100644, NULL), 0, "NFC name");
	CHECK_RC(ntfs_create(d, "cafe\xcc\x81-norm.txt", 0100644, NULL), 0, "NFD name");
	CHECK_RC(ntfs_lookup(d, "caf\xc3\xa9-norm.txt", &ni), 0, "lookup NFC");
	if (ni) ntfs_inode_put(ni);
	CHECK_RC(ntfs_lookup(d, "cafe\xcc\x81-norm.txt", &ni), 0, "lookup NFD");
	if (ni) ntfs_inode_put(ni);
	/* The two are independent: removing one leaves the other. */
	CHECK_RC(ntfs_unlink(d, "caf\xc3\xa9-norm.txt"), 0, "unlink NFC");
	CHECK_RC(ntfs_lookup(d, "caf\xc3\xa9-norm.txt", &ni), -ENOENT, "NFC gone");
	if (ni) ntfs_inode_put(ni);
	CHECK_RC(ntfs_lookup(d, "cafe\xcc\x81-norm.txt", &ni), 0, "NFD survived");
	if (ni) ntfs_inode_put(ni);

	list_dir(d, &l);
	CHECK(listed(&l, "cafe\xcc\x81-norm.txt"));
	CHECK(!listed(&l, "caf\xc3\xa9-norm.txt"));

	/* Ill-formed UTF-8 is rejected by the conversion, not silently
	 * substituted with U+FFFD: a name nobody typed is worse than an error. */
	CHECK_RC(ntfs_create(d, "bad\xff\xfe.txt", 0100644, NULL), -EILSEQ,
		 "ill-formed UTF-8");
	CHECK_RC(ntfs_create(d, "lone\xed\xa0\x80.txt", 0100644, NULL), -EILSEQ,
		 "UTF-8 encoded lone surrogate");

	ntfs_inode_put(d);
	ntfs_inode_put(root);
	CHECK(ntfs_unmount(vol) == 0);
	structural_check("test_long_and_unicode_names");
	unlink(scratch);
	printf("test_long_and_unicode_names\n");
}

/* ---- 5. xattrs are alternate data streams --------------------------- */

#define BIG_XATTR_SIZE (300 * 1024)

static void test_xattr_as_stream(void)
{
	ntfs_volume_t *vol;
	ntfs_inode_t *root, *ni;
	struct namelist l;
	struct ntfs_attr at;
	static char big[BIG_XATTR_SIZE], readback[BIG_XATTR_SIZE];
	char buf[4096];
	size_t len;
	int i, n;

	if (copy_fixture("names.img", "xattr") != 0) {
		fprintf(stderr, "SKIP test_xattr_as_stream: no fixture\n");
		return;
	}
	if (mount_scratch(0, &vol, &root)) {
		fprintf(stderr, "FAIL: could not mount for the xattr test\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	CHECK(ntfs_create(root, "xattrfile.bin", 0100644, &ni) == 0);
	if (!ni) {
		ntfs_inode_put(root);
		ntfs_unmount(vol);
		unlink(scratch);
		return;
	}
	CHECK(ntfs_write(ni, "main stream", 11, 0) == 11);

	/* A fresh file has no xattrs and no stream to find. */
	len = 12345;
	CHECK_RC(ntfs_listxattr(ni, NULL, 0, &len), 0, "listxattr on a bare file");
	CHECK_RC((int)len, 0, "listxattr length on a bare file");
	CHECK_RC(ntfs_getxattr(ni, "user.absent", NULL, 0, &len), -ENOATTR,
		 "getxattr of a name that is not there");
	CHECK_RC(ntfs_removexattr(ni, "user.absent"), -ENOATTR,
		 "removexattr of a name that is not there");

	/* set / get round trip. */
	CHECK_RC(ntfs_setxattr(ni, "com.apple.FinderInfo",
			       "0123456789abcdef0123456789abcdef", 32, 0), 0,
		 "set com.apple.FinderInfo");
	len = 0;
	CHECK_RC(ntfs_getxattr(ni, "com.apple.FinderInfo", NULL, 0, &len), 0,
		 "size query");
	CHECK_RC((int)len, 32, "size query result");
	memset(buf, 0, sizeof(buf));
	CHECK_RC(ntfs_getxattr(ni, "com.apple.FinderInfo", buf, sizeof(buf), &len), 0,
		 "read back");
	CHECK_RC((int)len, 32, "read back length");
	CHECK(memcmp(buf, "0123456789abcdef0123456789abcdef", 32) == 0);
	/* A buffer that is too small must say so, not truncate. */
	CHECK_RC(ntfs_getxattr(ni, "com.apple.FinderInfo", buf, 10, &len), -ERANGE,
		 "short buffer");

	/* An empty value is a real xattr, not a missing one: Finder writes
	 * zero-length values and expects them to list and read back. */
	CHECK_RC(ntfs_setxattr(ni, "user.empty", NULL, 0, 0), 0, "set an empty value");
	len = 999;
	CHECK_RC(ntfs_getxattr(ni, "user.empty", buf, sizeof(buf), &len), 0,
		 "get an empty value");
	CHECK_RC((int)len, 0, "empty value length");

	/*
	 * A value far past what fits in the base MFT record. The core creates
	 * the stream resident and empty and lets the write make it
	 * non-resident, so this is the path that allocates clusters for an
	 * xattr, and the one ntfsck below is really checking.
	 */
	for (i = 0; i < BIG_XATTR_SIZE; i++)
		big[i] = (char)(i * 31 + 7);
	CHECK_RC(ntfs_setxattr(ni, "user.large", big, sizeof(big), 0), 0,
		 "set a 300 KiB value");
	len = 0;
	CHECK_RC(ntfs_getxattr(ni, "user.large", NULL, 0, &len), 0, "large size query");
	CHECK_RC((int)len, BIG_XATTR_SIZE, "large size query result");
	CHECK_RC(ntfs_getxattr(ni, "user.large", readback, sizeof(readback), &len), 0,
		 "large read back");
	CHECK(memcmp(readback, big, sizeof(big)) == 0);

	/*
	 * The documented names do what they say. Using the public macros rather
	 * than literals means this also checks that the header's values are the
	 * ones the compiled core acts on: if ntfs_xattr_flags.h and the core ever
	 * disagreed, CREATE below would come back as a REPLACE and fail here.
	 */
	CHECK_RC(ntfs_setxattr(ni, "user.created", "abc", 3, NTFS_XATTR_CREATE), 0,
		 "CREATE on a name that is free");
	CHECK_RC(ntfs_setxattr(ni, "user.created", "xyz", 3, NTFS_XATTR_CREATE), -EEXIST,
		 "CREATE on a name that exists");
	CHECK_RC(ntfs_setxattr(ni, "user.created", "defgh", 5, NTFS_XATTR_REPLACE), 0,
		 "REPLACE on a name that exists");
	CHECK_RC(ntfs_setxattr(ni, "user.notthere", "x", 1, NTFS_XATTR_REPLACE), -ENOATTR,
		 "REPLACE on a name that is free");
	memset(buf, 0, sizeof(buf));
	CHECK_RC(ntfs_getxattr(ni, "user.created", buf, sizeof(buf), &len), 0,
		 "read back after REPLACE");
	CHECK_RC((int)len, 5, "REPLACE shortened the value");
	CHECK(memcmp(buf, "defgh", 5) == 0);
	/* Flags 0 is "create or replace", the default for both cases. */
	CHECK_RC(ntfs_setxattr(ni, "user.created", "z", 1, 0), 0, "flags 0 replaces");
	CHECK_RC(ntfs_setxattr(ni, "user.flagless", "z", 1, 0), 0, "flags 0 creates");

	/*
	 * Pin the flag NUMBERS, not just the names, with bare literals -- so this
	 * still fails if every named copy is renumbered together. The static
	 * assertions at the top of this file catch the same drift at compile
	 * time; these catch a core that stopped agreeing with its own header.
	 *
	 * Written as the mistake a Darwin caller makes: passing Darwin's
	 * XATTR_CREATE (2) reaches the core as REPLACE and fails with ENOATTR
	 * rather than creating, and Darwin's XATTR_REPLACE (4) reaches it as no
	 * flags at all and creates the name it was supposed to require.
	 */
	CHECK_RC(ntfs_setxattr(ni, "user.flagprobe", "a", 1, DARWIN_XATTR_CREATE), -ENOATTR,
		 "Darwin's CREATE (2) is this core's REPLACE");
	CHECK_RC(ntfs_setxattr(ni, "user.flagprobe", "a", 1, 1), 0,
		 "flag value 1 behaves as CREATE");
	CHECK_RC(ntfs_setxattr(ni, "user.flagprobe", "b", 1, 1), -EEXIST,
		 "flag value 1 refuses an existing name");
	CHECK_RC(ntfs_setxattr(ni, "user.flagprobe", "c", 1, 2), 0,
		 "flag value 2 replaces an existing name");
	/* Darwin's REPLACE (4) is not a flag the core knows: it creates. Removed
	 * again so the listxattr expectations below stay about listing. */
	CHECK_RC(ntfs_setxattr(ni, "user.darwinreplace", "d", 1, DARWIN_XATTR_REPLACE), 0,
		 "Darwin's REPLACE (4) reaches the core as no flags and creates");
	CHECK_RC(ntfs_removexattr(ni, "user.darwinreplace"), 0, "drop the probe");

	/*
	 * xattr names go through the same Windows-name policy as filenames,
	 * because a stream name is what follows the colon in "file:stream" on
	 * Windows: a colon inside one would produce a name Windows cannot parse.
	 */
	CHECK_RC(ntfs_setxattr(ni, "user:colon", "x", 1, 0), -EINVAL,
		 "xattr name containing a colon");
	CHECK_RC(ntfs_setxattr(ni, "", "x", 1, 0), -EINVAL, "empty xattr name");

	/* listxattr: size query then the names, each NUL-terminated. */
	len = 0;
	CHECK_RC(ntfs_listxattr(ni, NULL, 0, &len), 0, "list size query");
	{
		size_t want = strlen("com.apple.FinderInfo") + 1 + strlen("user.empty") + 1 +
			      strlen("user.large") + 1 + strlen("user.created") + 1 +
			      strlen("user.flagless") + 1 + strlen("user.flagprobe") + 1;
		CHECK_RC((int)len, (int)want, "list size query result");
	}
	memset(buf, 0, sizeof(buf));
	CHECK_RC(ntfs_listxattr(ni, buf, sizeof(buf), &len), 0, "list");
	{
		static const char *const want[] = {
			"com.apple.FinderInfo", "user.empty", "user.large",
			"user.created", "user.flagless", "user.flagprobe",
		};
		size_t off;
		int found = 0;

		for (i = 0; i < (int)ARRAY_LEN(want); i++) {
			bool hit = false;

			for (off = 0; off < len; off += strlen(buf + off) + 1)
				if (!strcmp(buf + off, want[i]))
					hit = true;
			checks++;
			if (!hit) {
				failures++;
				fprintf(stderr, "FAIL %s:%d: '%s' missing from listxattr\n",
					__FILE__, __LINE__, want[i]);
			}
		}
		for (off = 0; off < len; off += strlen(buf + off) + 1)
			found++;
		CHECK_RC(found, (int)ARRAY_LEN(want), "listxattr entry count");
	}
	/* The unnamed $DATA stream is the file's contents and must never be
	 * listed as an xattr. */
	{
		size_t off;

		for (off = 0; off < len; off += strlen(buf + off) + 1)
			CHECK(buf[off] != 0);
	}

	/* The named streams show up in the inode's attributes. */
	CHECK(ntfs_getattr(ni, &at) == 0);
	CHECK(at.has_ads);
	CHECK_RC((int)at.size, 11, "the unnamed stream still holds the file data");
	CHECK(ntfs_read(ni, buf, 11, 0) == 11);
	CHECK(memcmp(buf, "main stream", 11) == 0);

	/* remove. */
	CHECK_RC(ntfs_removexattr(ni, "user.large"), 0, "remove");
	CHECK_RC(ntfs_removexattr(ni, "user.large"), -ENOATTR, "remove twice");
	CHECK_RC(ntfs_getxattr(ni, "user.large", NULL, 0, &len), -ENOATTR,
		 "get after remove");
	CHECK_RC(ntfs_listxattr(ni, buf, sizeof(buf), &len), 0, "list after remove");
	{
		size_t off;

		for (off = 0; off < len; off += strlen(buf + off) + 1)
			CHECK(strcmp(buf + off, "user.large") != 0);
	}
	/* Removing an xattr must not touch the file's own data. */
	CHECK(ntfs_read(ni, buf, 11, 0) == 11);
	CHECK(memcmp(buf, "main stream", 11) == 0);

	/*
	 * No AppleDouble anywhere. The core has no notion of "._" and this is
	 * what keeps it that way: a sidecar would be a second directory entry
	 * per file, copied around by every user who drags the folder.
	 */
	list_dir(root, &l);
	for (i = 0; i < l.n; i++) {
		checks++;
		if (!strncmp(l.names[i], "._", 2)) {
			failures++;
			fprintf(stderr, "FAIL %s:%d: AppleDouble sidecar '%s' in the root\n",
				__FILE__, __LINE__, l.names[i]);
		}
	}
	CHECK(!listed(&l, "._xattrfile.bin"));
	CHECK(listed(&l, "xattrfile.bin"));

	ntfs_inode_put(ni);
	ntfs_inode_put(root);
	CHECK(ntfs_unmount(vol) == 0);

	/*
	 * The independent reader. ntfscat knows nothing about xattrs; -n asks
	 * for a named $DATA stream. If it can read the value back, the xattr
	 * really is an alternate data stream on the volume and not a private
	 * encoding only this driver understands.
	 */
	memset(buf, 0, sizeof(buf));
	n = ntfscat_stream("/xattrfile.bin", "user.created", buf, sizeof(buf));
	if (n < 0) {
		fprintf(stderr, "SKIP ntfscat stream check: ntfsprogs not built\n");
	} else {
		CHECK_RC(n, 1, "ntfscat read the named stream");
		CHECK(buf[0] == 'z');
		memset(buf, 0, sizeof(buf));
		n = ntfscat_stream("/xattrfile.bin", "com.apple.FinderInfo", buf, sizeof(buf));
		CHECK_RC(n, 32, "ntfscat read com.apple.FinderInfo as a stream");
		CHECK(memcmp(buf, "0123456789abcdef0123456789abcdef", 32) == 0);
		/* The removed one is gone from the volume, not just from our list. */
		n = ntfscat_stream("/xattrfile.bin", "user.large", buf, sizeof(buf));
		CHECK_RC(n, 0, "ntfscat finds nothing for the removed stream");
	}
	CHECK(!ntfsls_root_has("._xattrfile.bin"));

	structural_check("test_xattr_as_stream");
	unlink(scratch);
	printf("test_xattr_as_stream\n");
}

int main(void)
{
	test_windows_illegal_rejected();
	test_windows_illegal_allowed();
	test_case_insensitive_preserving();
	test_long_and_unicode_names();
	test_xattr_as_stream();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
