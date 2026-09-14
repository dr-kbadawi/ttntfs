// SPDX-License-Identifier: GPL-2.0
/*
 * test_bigdir.c - the directory index B-tree, once a directory is big enough
 * to have one.
 *
 * Until this file, nothing exercised a directory past a handful of entries, so
 * the whole of the B-tree machinery in core/ntfs/index.c -- ntfs_ib_split(),
 * ntfs_ir_reparent(), ntfs_index_rm_node(), ntfs_index_rm_leaf() and
 * ntfs_ih_takeout() -- ran only on real disks and only in the field. That code
 * is worth testing because of how it fails: a mis-split index does not lose the
 * file, it loses the *name*. The MFT record, its data and its clusters are all
 * still there, and nothing above the B-tree can tell. lookup() returns -ENOENT,
 * readdir() skips it, and the volume still looks structurally sane to anything
 * that is not a B-tree checker. That is why every phase here ends in
 * `ntfsck -n`: a read-back comparison would agree with the driver's own broken
 * tree, and a structural checker will not.
 *
 * What each test covers:
 *
 *   index_root_boundary   the move out of the resident $INDEX_ROOT into an
 *                         $INDEX_ALLOCATION (ntfs_ir_reparent), found by
 *                         watching the on-disk MFT record rather than assumed.
 *   multilevel_growth     a few thousand entries: many ntfs_ib_split() calls,
 *                         a root that itself has to be reparented, and the
 *                         resulting tree read back after a remount.
 *   delete_orders         deleting back to empty in creation, reverse and
 *                         shuffled order -- what drives ntfs_index_rm_leaf()
 *                         and the collapse back into the index root.
 *   delete_middle         deleting a contiguous run out of the middle, so a
 *                         node empties while both its neighbours stay, which
 *                         is the path that makes a parent give up an entry
 *                         (ntfs_ih_takeout / ntfs_ih_reparent_end).
 *   collation_shapes      keys that collate adjacently (one long shared
 *                         prefix) and keys spread across the collation order,
 *                         inserted ascending, descending and shuffled. The
 *                         split point is chosen by key order, so the shape of
 *                         the key space decides which code path runs.
 *
 * Set NTFS_BIGDIR_HEAVY=1 for a slower run (see HEAVY_ENTRIES below). It is off
 * by default because this suite runs on every push.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
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

#define CHECKF(cond, ...) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
	}								\
} while (0)

/* Entries for the growth test. 3000 is enough to force three levels of index
 * blocks below the root (measured: see the header comment of
 * test_multilevel_growth); tens of thousands buys nothing but CI minutes. */
#define GROW_ENTRIES	3000
#define HEAVY_ENTRIES	20000
#define DELETE_ENTRIES	800
#define MIDDLE_ENTRIES	1200
#define COLLATE_ENTRIES	600

#define NAME_MAX_LEN	48

static int grow_entries(void)
{
	const char *h = getenv("NTFS_BIGDIR_HEAVY");
	return (h && *h && strcmp(h, "0")) ? HEAVY_ENTRIES : GROW_ENTRIES;
}

/* ---- the two external tools ------------------------------------------ */

static const char *tool_path(const char *env, const char *fallback)
{
	const char *p = getenv(env);
	return (p && *p) ? p : fallback;
}

static bool have_tool(const char *path)
{
	return access(path, X_OK) == 0;
}

static int run_quiet(const char *cmd)
{
	char buf[2048];
	int rc;

	snprintf(buf, sizeof(buf), "%s >/dev/null 2>&1", cmd);
	rc = system(buf);
	if (rc < 0 || !WIFEXITED(rc))
		return -1;
	return WEXITSTATUS(rc);
}

/*
 * ntfsck from ntfsprogs-plus is the only structural checker available here, and
 * it is the assertion that makes this file worth having. ALWAYS -n: without it
 * ntfsck repairs the image, and a test that repairs what it is measuring
 * measures the repair.
 */
static bool fsck_ok(const char *img, const char *what)
{
	static const char *ck;
	char cmd[2048];
	int rc;

	if (!ck)
		ck = tool_path("NTFS_NTFSCK", "tools/.local-plus/sbin/ntfsck");
	if (!have_tool(ck))
		return true;		/* reported once by fsck_available() */

	snprintf(cmd, sizeof(cmd), "'%s' -n '%s'", ck, img);
	rc = run_quiet(cmd);
	checks++;
	if (rc != 0) {
		failures++;
		fprintf(stderr, "FAIL ntfsck -n reported %d after %s (%s)\n",
			rc, what, img);
		return false;
	}
	return true;
}

/* A failing run leaves its images behind: a B-tree bug is much easier to read
 * off the image than to reproduce. A passing run leaves nothing. */
static void cleanup_image(const char *path)
{
	if (failures)
		fprintf(stderr, "  keeping %s for inspection\n", path);
	else
		unlink(path);
}

static bool fsck_available(void)
{
	return have_tool(tool_path("NTFS_NTFSCK", "tools/.local-plus/sbin/ntfsck"));
}

/* A zero-filled file, then mkntfs over it. mkntfs needs -F because the target
 * is a plain file rather than a block device. */
static int make_image(const char *path, unsigned mb)
{
	const char *mk = tool_path("NTFS_MKNTFS", "tools/.local/sbin/mkntfs");
	char cmd[2048];
	int fd;

	if (!have_tool(mk))
		return -ENOENT;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -errno;
	if (ftruncate(fd, (off_t)mb << 20) != 0) {
		close(fd);
		return -errno;
	}
	close(fd);
	snprintf(cmd, sizeof(cmd), "'%s' -Q -F -L BIGDIR '%s'", mk, path);
	return run_quiet(cmd) == 0 ? 0 : -EIO;
}

/* One mkntfs run for the whole suite; every test starts from a copy of it, so
 * no test can see another's leftovers. */
static char base_img[512];

static int copy_base(const char *dst)
{
	char buf[1 << 16];
	ssize_t n;
	int in, out;

	in = open(base_img, O_RDONLY);
	if (in < 0)
		return -errno;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) { close(in); return -errno; }
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n) {
			close(in); close(out); return -EIO;
		}
	close(in);
	close(out);
	return 0;
}

/* ---- reading the index's real shape off the image --------------------- */
/*
 * The public ABI cannot say whether a directory's index is still resident, how
 * many index blocks it has, or how deep it is -- and those are exactly the
 * facts under test. So read the directory's MFT record straight out of the
 * image file. ntfs_image (core/logfile) already locates $MFT; the rest is
 * attribute and run-list decoding, which is about sixty lines.
 */

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t g32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t g64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

/* Undo the multi-sector fixup: put the saved tail words back. */
static int unfixup(uint8_t *rec, uint32_t size, uint32_t sector)
{
	uint16_t uofs = g16(rec + 4), ucnt = g16(rec + 6);

	if (!ucnt || (uint32_t)uofs + ucnt * 2u > size)
		return -1;
	if ((ucnt - 1u) * sector != size)
		return -1;
	for (uint16_t i = 1; i < ucnt; i++) {
		uint8_t *tail = rec + i * sector - 2;

		if (g16(tail) != g16(rec + uofs))
			return -1;	/* torn write */
		tail[0] = rec[uofs + i * 2];
		tail[1] = rec[uofs + i * 2 + 1];
	}
	return 0;
}

struct run { uint64_t vcn, lcn, len; };

static int decode_runs(const uint8_t *p, const uint8_t *end, struct run *out, int max)
{
	uint64_t vcn = 0;
	int64_t lcn = 0;
	int n = 0;

	while (p < end && *p) {
		int lb = *p & 0xf, ob = (*p >> 4) & 0xf;
		uint64_t len = 0;
		int64_t delta = 0;

		p++;
		if (!lb || p + lb + ob > end)
			return -1;
		for (int i = lb - 1; i >= 0; i--)
			len = (len << 8) | p[i];
		p += lb;
		if (ob) {
			delta = (int8_t)p[ob - 1];
			for (int i = ob - 2; i >= 0; i--)
				delta = (delta << 8) | p[i];
			p += ob;
		}
		lcn += delta;
		if (n >= max)
			return -1;
		out[n].vcn = vcn;
		out[n].lcn = ob ? (uint64_t)lcn : UINT64_MAX;	/* no offset: sparse */
		out[n].len = len;
		n++;
		vcn += len;
	}
	return n;
}

struct idx_shape {
	bool large_index;	/* $INDEX_ROOT flagged LARGE_INDEX */
	bool has_alloc;		/* an $INDEX_ALLOCATION attribute exists */
	int height;		/* 1 = index root only; +1 per index-block level
				 * on the leftmost root-to-leaf path */
	int nblocks;		/* $INDEX_ALLOCATION size / index block size */
	uint32_t ib_size;
};

/* Layout constants used below, from core/ntfs/layout.h: attribute types
 * $INDEX_ROOT 0x90 and $INDEX_ALLOCATION 0xa0; index_header.flags bit 0 is
 * LARGE_INDEX in the root and INDEX_NODE in a block; index_entry.flags bit 0
 * is INDEX_ENTRY_NODE, and a node entry's child VCN is its last 8 bytes. */
static int read_shape(const char *path, uint64_t mft_no, struct idx_shape *out)
{
	struct ntfs_image img;
	uint8_t *rec = NULL, *blk = NULL;
	struct run runs[64];
	int nruns = 0;
	int err;

	memset(out, 0, sizeof(*out));
	err = ntfs_image_open(&img, path, false, 0, 0);
	if (err)
		return err;

	rec = malloc(img.mft_record_size);
	if (!rec) { err = -ENOMEM; goto out; }
	{
		uint64_t off = ntfs_image_map(img.mft_runs, img.n_mft_runs,
					      img.cluster_size,
					      mft_no * img.mft_record_size);
		if (off == UINT64_MAX) { err = -EIO; goto out; }
		if (pread(img.fd, rec, img.mft_record_size, (off_t)off) !=
		    (ssize_t)img.mft_record_size) { err = -EIO; goto out; }
	}
	if (memcmp(rec, "FILE", 4)) { err = -EIO; goto out; }
	if (unfixup(rec, img.mft_record_size, img.sector_size)) { err = -EIO; goto out; }

	{
	uint32_t used = g32(rec + 0x18);
	uint8_t *a = rec + g16(rec + 0x14);
	uint8_t *root = NULL, *ia = NULL;

	while ((uint32_t)(a - rec) + 8 <= used) {
		uint32_t type = g32(a), len = g32(a + 4);

		if (type == 0xffffffffu || !len)
			break;
		if (type == 0x90)
			root = a + g16(a + 0x14);	/* resident value */
		else if (type == 0xa0)
			ia = a;
		a += len;
	}
	if (!root) { err = -ENOENT; goto out; }

	/* index_root: type, collation, index_block_size, clusters_per_block +
	 * 3 reserved = 16 bytes, then the index_header. */
	out->ib_size = g32(root + 8);
	out->large_index = (root + 16)[12] & 1;
	out->has_alloc = ia != NULL;
	out->height = 1;
	if (!ia)
		goto out;

	nruns = decode_runs(ia + g16(ia + 0x20), rec + img.mft_record_size, runs, 64);
	if (nruns < 0) { err = -EIO; goto out; }
	out->nblocks = (int)(g64(ia + 0x30) / out->ib_size);

	{
	/* VCNs in an index are counted in clusters, unless a cluster is bigger
	 * than an index block, in which case they are counted in sectors. */
	uint32_t vcn_size = (img.cluster_size <= out->ib_size) ?
			     img.cluster_size : img.sector_size;
	uint8_t *ih = root + 16;
	uint8_t *ie = ih + g32(ih);

	blk = malloc(out->ib_size);
	if (!blk) { err = -ENOMEM; goto out; }
	while (g16(ie + 12) & 1) {		/* INDEX_ENTRY_NODE */
		uint64_t vcn = g64(ie + g16(ie + 8) - 8);
		uint64_t vbo = vcn * vcn_size, bo = UINT64_MAX;

		for (int i = 0; i < nruns; i++) {
			uint64_t start = runs[i].vcn * img.cluster_size;
			uint64_t end = start + runs[i].len * img.cluster_size;

			if (vbo >= start && vbo < end && runs[i].lcn != UINT64_MAX) {
				bo = runs[i].lcn * img.cluster_size + (vbo - start);
				break;
			}
		}
		if (bo == UINT64_MAX) { err = -EIO; goto out; }
		if (pread(img.fd, blk, out->ib_size, (off_t)bo) !=
		    (ssize_t)out->ib_size) { err = -EIO; goto out; }
		if (memcmp(blk, "INDX", 4)) { err = -EIO; goto out; }
		if (unfixup(blk, out->ib_size, img.sector_size)) { err = -EIO; goto out; }
		out->height++;
		ih = blk + 24;		/* index_block header is 24 bytes */
		ie = ih + g32(ih);
		if (out->height > 16) { err = -ELOOP; goto out; }
	}
	}
	}
out:
	free(rec);
	free(blk);
	ntfs_image_close(&img);
	return err;
}

/* ---- mount / unmount -------------------------------------------------- */

struct session {
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root;
};

static int session_open(struct session *s, const char *path)
{
	struct ntfs_mount_options o = { .flags = 0, .uid = 0, .gid = 0,
					.fmask = 022, .dmask = 022 };
	int err;

	memset(s, 0, sizeof(*s));
	s->dev = ntfs_bdev_open_path(path, false);
	if (!s->dev)
		return -ENOENT;
	err = ntfs_mount(s->dev, &o, &s->vol);
	if (err) {
		ntfs_bdev_close(s->dev);
		s->dev = NULL;
		return err;
	}
	err = ntfs_volume_root(s->vol, &s->root);
	if (err) {
		ntfs_unmount(s->vol);
		ntfs_bdev_close(s->dev);
		memset(s, 0, sizeof(*s));
	}
	return err;
}

static void session_close(struct session *s)
{
	if (s->root)
		ntfs_inode_put(s->root);
	if (s->vol)
		ntfs_unmount(s->vol);
	if (s->dev)
		ntfs_bdev_close(s->dev);
	memset(s, 0, sizeof(*s));
}

/* ---- the expected set of names ---------------------------------------- */
/*
 * Every phase asks the same two questions: is every live name findable, and
 * does readdir return exactly the live names -- nothing missing, nothing twice.
 * A mis-split index shows up as one or the other.
 */

struct nameset {
	char (*names)[NAME_MAX_LEN];
	bool *live;
	int *sorted;			/* indices, ordered by strcmp(name) */
	unsigned char *seen;
	int n, nlive;
};

static struct nameset *g_sort_set;	/* qsort/bsearch comparator context */

static int cmp_idx(const void *a, const void *b)
{
	const struct nameset *s = g_sort_set;

	return strcmp(s->names[*(const int *)a], s->names[*(const int *)b]);
}

static int cmp_key(const void *key, const void *elem)
{
	const struct nameset *s = g_sort_set;

	return strcmp((const char *)key, s->names[*(const int *)elem]);
}

static struct nameset *nameset_new(int n)
{
	struct nameset *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	s->names = calloc((size_t)n, NAME_MAX_LEN);
	s->live = calloc((size_t)n, sizeof(bool));
	s->sorted = calloc((size_t)n, sizeof(int));
	s->seen = calloc((size_t)n, 1);
	s->n = n;
	if (!s->names || !s->live || !s->sorted || !s->seen) {
		free(s->names); free(s->live); free(s->sorted); free(s->seen);
		free(s);
		return NULL;
	}
	return s;
}

static void nameset_free(struct nameset *s)
{
	if (!s)
		return;
	free(s->names); free(s->live); free(s->sorted); free(s->seen);
	free(s);
}

static void nameset_seal(struct nameset *s)
{
	for (int i = 0; i < s->n; i++)
		s->sorted[i] = i;
	g_sort_set = s;
	qsort(s->sorted, (size_t)s->n, sizeof(int), cmp_idx);
}

static int nameset_find(struct nameset *s, const char *name)
{
	int *hit;

	g_sort_set = s;
	hit = bsearch(name, s->sorted, (size_t)s->n, sizeof(int), cmp_key);
	return hit ? *hit : -1;
}

struct rd_ctx {
	struct nameset *set;
	int extra;		/* names readdir returned that we never created */
	int dupes;
	int dots;
};

static int rd_cb(const struct ntfs_dirent *ent, void *vctx)
{
	struct rd_ctx *c = vctx;
	char name[NAME_MAX_LEN];
	int idx;

	if (ent->name_len >= sizeof(name)) {
		c->extra++;
		return 0;
	}
	memcpy(name, ent->name, ent->name_len);
	name[ent->name_len] = '\0';
	if (!strcmp(name, ".") || !strcmp(name, "..")) {
		c->dots++;
		return 0;
	}
	idx = nameset_find(c->set, name);
	if (idx < 0 || !c->set->live[idx]) {
		c->extra++;
		fprintf(stderr, "  readdir returned unexpected name '%s'\n", name);
		return 0;
	}
	if (c->set->seen[idx]) {
		c->dupes++;
		fprintf(stderr, "  readdir returned '%s' twice\n", name);
		return 0;
	}
	c->set->seen[idx] = 1;
	return 0;
}

/*
 * readdir must return the live set exactly: no duplicates, nothing missing,
 * nothing invented. This is the assertion a lost index entry fails.
 *
 * Exact equality is only fair because of two things ntfs_filldir() does, both
 * of which these tests stay clear of rather than assert: it drops entries in
 * the DOS name space, and it drops anything below FILE_first_user unless the
 * volume was mounted with show_sys_files. Every name here is created through
 * ntfs_create(), which writes a single FILE_NAME_POSIX name (core/vfs/namei.c),
 * so there are no 8.3 companions to be filtered; and every test works in a
 * subdirectory, which holds no system files. In the root directory the counts
 * would not match, and that would be the mount option talking, not the B-tree.
 */
static void verify_readdir(ntfs_inode_t *dir, struct nameset *s, const char *where)
{
	struct rd_ctx c = { .set = s };
	uint64_t cookie = 0;
	bool eof = false;
	int seen = 0, err;

	memset(s->seen, 0, (size_t)s->n);
	while (!eof) {
		err = ntfs_readdir(dir, &cookie, false, rd_cb, &c, &eof);
		CHECKF(err == 0, "%s: readdir returned %d", where, err);
		if (err)
			return;
	}
	for (int i = 0; i < s->n; i++)
		if (s->seen[i])
			seen++;
	CHECKF(seen == s->nlive, "%s: readdir saw %d of %d live names (%d missing)",
	       where, seen, s->nlive, s->nlive - seen);
	CHECKF(c.extra == 0, "%s: readdir returned %d unexpected names", where, c.extra);
	CHECKF(c.dupes == 0, "%s: readdir returned %d duplicate names", where, c.dupes);
	CHECKF(c.dots == 2, "%s: readdir emitted %d dot entries, expected . and ..",
	       where, c.dots);
	if (seen != s->nlive) {
		int shown = 0;

		for (int i = 0; i < s->n && shown < 5; i++)
			if (s->live[i] && !s->seen[i]) {
				fprintf(stderr, "  missing from readdir: '%s'\n", s->names[i]);
				shown++;
			}
	}
}

/* Every live name must be findable, and no dead name may be. One CHECK for the
 * whole sweep: n separate ones would bury the output. */
static void verify_lookup(ntfs_inode_t *dir, struct nameset *s, const char *where)
{
	int missing = 0, resurrected = 0, shown = 0;

	for (int i = 0; i < s->n; i++) {
		ntfs_inode_t *ni = NULL;
		int err = ntfs_lookup(dir, s->names[i], &ni);

		if (!err)
			ntfs_inode_put(ni);
		if (s->live[i] && err) {
			missing++;
			if (shown++ < 5)
				fprintf(stderr, "  lookup('%s') returned %d, expected success\n",
					s->names[i], err);
		} else if (!s->live[i] && !err) {
			resurrected++;
			if (shown++ < 5)
				fprintf(stderr, "  lookup('%s') succeeded for a deleted name\n",
					s->names[i]);
		}
	}
	CHECKF(missing == 0, "%s: %d of %d live names could not be looked up",
	       where, missing, s->nlive);
	CHECKF(resurrected == 0, "%s: %d deleted names still look up", where, resurrected);
}

/* ---- scratch paths ---------------------------------------------------- */

static char scratch[512];

static void scratch_path(const char *tag)
{
	snprintf(scratch, sizeof(scratch), "/tmp/ttntfs-bigdir-%d-%s.img", (int)getpid(), tag);
}

/* A deterministic shuffle: the same permutation on every run and every host, so
 * a failure here is reproducible. */
static void shuffle(int *v, int n, unsigned seed)
{
	unsigned s = seed;

	for (int i = n - 1; i > 0; i--) {
		int j;

		s = s * 1103515245u + 12345u;
		j = (int)((s >> 16) % (unsigned)(i + 1));
		int t = v[i]; v[i] = v[j]; v[j] = t;
	}
}

/* ---- 1. where the index leaves the resident index root ---------------- */

/*
 * The transition is not a fixed entry count: it happens when $INDEX_ROOT can no
 * longer grow inside the MFT record, so it depends on what else that record
 * holds. Measured here on 2026-09-14 with a 1024-byte MFT record, the
 * eleven-character names below, and a directory created by this driver: the
 * FOURTH entry moves the index out. Name length barely matters -- 4-character
 * names also give 4, and 40-character names give 3. That is far earlier than
 * the "few tens of entries" a directory
 * made by Windows manages, and the reason is visible in the record: alongside
 * $STANDARD_INFORMATION, $FILE_NAME and a resident $SECURITY_DESCRIPTOR, every
 * inode this port creates also carries the WSL $EA / $EA_INFORMATION pair
 * written by ntfs_ea_set_wsl_inode() (core/vfs/namei.c), which costs another
 * 120 bytes. Nothing here says that is wrong -- it is just what the port does,
 * and the point of this test is that the reparent runs early and correctly, not
 * that it runs at any particular count. So the assertion is a range, and the
 * measured value is printed.
 */
static void test_index_root_boundary(void)
{
	struct session s;
	ntfs_inode_t *dir = NULL, *f = NULL;
	struct nameset *set;
	uint64_t dir_no;
	int moved_at = 0, err;
	const int n = 40;

	scratch_path("boundary");
	if (copy_base(scratch) != 0) {
		fprintf(stderr, "SKIP test_index_root_boundary: no base image\n");
		return;
	}
	set = nameset_new(n);
	if (!set) { unlink(scratch); return; }
	for (int i = 0; i < n; i++)
		snprintf(set->names[i], NAME_MAX_LEN, "entry-%05d", i);
	nameset_seal(set);

	err = session_open(&s, scratch);
	CHECKF(err == 0, "mount failed: %d", err);
	if (err) goto out;
	err = ntfs_mkdir(s.root, "grow", 0040755, &dir);
	CHECKF(err == 0, "mkdir failed: %d", err);
	if (err) goto close;
	dir_no = ntfs_inode_number(dir);

	{
		struct idx_shape sh;

		CHECK(ntfs_volume_sync(s.vol) == 0);
		err = read_shape(scratch, dir_no, &sh);
		CHECKF(err == 0, "read_shape on the empty directory failed: %d", err);
		CHECK(!sh.has_alloc);		/* an empty directory is resident */
		CHECK(!sh.large_index);
		CHECK(sh.height == 1);
	}

	for (int i = 0; i < n; i++) {
		struct idx_shape sh;

		err = ntfs_create(dir, set->names[i], 0100644, &f);
		CHECKF(err == 0, "create('%s') failed: %d", set->names[i], err);
		if (err) goto close;
		ntfs_inode_put(f);
		set->live[i] = true;
		set->nlive++;

		CHECK(ntfs_volume_sync(s.vol) == 0);
		if (read_shape(scratch, dir_no, &sh) == 0) {
			if (!moved_at && sh.has_alloc) {
				moved_at = i + 1;
				CHECK(sh.large_index);
				CHECK(sh.height == 2);
				CHECK(sh.nblocks >= 1);
			}
		}
	}

	CHECKF(moved_at > 1 && moved_at <= n,
	       "the index never left $INDEX_ROOT in %d entries", n);
	printf("  index left $INDEX_ROOT at entry %d\n", moved_at);

	/* Everything created before and after the move must still be there. */
	verify_lookup(dir, set, "boundary/mounted");
	verify_readdir(dir, set, "boundary/mounted");

	ntfs_inode_put(dir);
	dir = NULL;
close:
	if (dir)
		ntfs_inode_put(dir);
	session_close(&s);
	fsck_ok(scratch, "growing past the index root");

	/* And after a remount, from disk rather than from cache. */
	if (session_open(&s, scratch) == 0) {
		if (ntfs_lookup(s.root, "grow", &dir) == 0) {
			verify_lookup(dir, set, "boundary/remounted");
			verify_readdir(dir, set, "boundary/remounted");
			ntfs_inode_put(dir);
		} else {
			CHECKF(false, "the directory itself vanished across a remount");
		}
		session_close(&s);
	}
out:
	nameset_free(set);
	cleanup_image(scratch);
	printf("test_index_root_boundary\n");
}

/* ---- 2. several thousand entries: splits, and a root that reparents --- */

/*
 * Measured on 2026-09-14 with these names (eleven characters), 4 KiB index
 * blocks and 1 KiB MFT records: the index leaves $INDEX_ROOT at entry 4, gains
 * a second level of index blocks (root -> node -> leaf) at entry 79, and a
 * third at entry 1459; 3000 entries end up four levels deep across 159 blocks.
 * So 3000 entries is comfortably past the point where a split has to promote a
 * median into an internal node rather than into the index root -- which is the
 * part of ntfs_ib_split() that a handful of entries never reaches. The
 * assertions below are ranges around those numbers, not the numbers: the exact
 * counts move with name length and index block size, the shape does not.
 */
static void test_multilevel_growth(void)
{
	struct session s;
	ntfs_inode_t *dir = NULL, *f = NULL;
	struct nameset *set;
	uint64_t dir_no = 0;
	const int n = grow_entries();
	int checkpoints[4], ncp = 0;
	int err;

	scratch_path("multilevel");
	/* 64 MiB holds ~3000 files comfortably; the heavy run needs more MFT. */
	if (n > GROW_ENTRIES) {
		if (make_image(scratch, 512) != 0) {
			fprintf(stderr, "SKIP test_multilevel_growth: mkntfs failed\n");
			return;
		}
	} else if (copy_base(scratch) != 0) {
		fprintf(stderr, "SKIP test_multilevel_growth: no base image\n");
		return;
	}

	set = nameset_new(n);
	if (!set) { unlink(scratch); return; }
	for (int i = 0; i < n; i++)
		snprintf(set->names[i], NAME_MAX_LEN, "file-%06d", i);
	nameset_seal(set);

	checkpoints[ncp++] = n / 20;
	checkpoints[ncp++] = n / 4;
	checkpoints[ncp++] = n / 2;
	checkpoints[ncp++] = n;

	err = session_open(&s, scratch);
	CHECKF(err == 0, "mount failed: %d", err);
	if (err) goto out;
	err = ntfs_mkdir(s.root, "big", 0040755, &dir);
	CHECKF(err == 0, "mkdir failed: %d", err);
	if (err) { session_close(&s); goto out; }
	dir_no = ntfs_inode_number(dir);

	for (int cp = 0; cp < ncp; cp++) {
		struct idx_shape sh;

		for (int i = set->nlive; i < checkpoints[cp]; i++) {
			err = ntfs_create(dir, set->names[i], 0100644, &f);
			if (err) {
				CHECKF(false, "create('%s') failed at entry %d: %d",
				       set->names[i], i, err);
				goto close;
			}
			ntfs_inode_put(f);
			set->live[i] = true;
			set->nlive++;
		}

		/* In cache first... */
		verify_lookup(dir, set, "growth/mounted");
		verify_readdir(dir, set, "growth/mounted");

		/* ...then on disk, which is the only view ntfsck and the next
		 * mount will ever see. */
		ntfs_inode_put(dir);
		dir = NULL;
		session_close(&s);
		fsck_ok(scratch, "growing a directory");

		err = read_shape(scratch, dir_no, &sh);
		CHECKF(err == 0, "read_shape at %d entries failed: %d", set->nlive, err);
		if (!err) {
			CHECK(sh.has_alloc);
			CHECK(sh.large_index);
			CHECKF(sh.height >= 2, "at %d entries the tree is still flat", set->nlive);
			if (cp == ncp - 1) {
				/* The whole point of the test: more than one
				 * level of index blocks below the root, so a
				 * split has to promote its median into an
				 * internal node, not into $INDEX_ROOT. */
				CHECKF(sh.height >= 3,
				       "%d entries produced a tree only %d level(s) deep; "
				       "ntfs_ib_split never promoted into an internal node",
				       set->nlive, sh.height);
				printf("  %d entries: %d levels, %d index blocks\n",
				       set->nlive, sh.height, sh.nblocks);
			}
		}

		err = session_open(&s, scratch);
		CHECKF(err == 0, "remount at %d entries failed: %d", set->nlive, err);
		if (err) goto out;
		err = ntfs_lookup(s.root, "big", &dir);
		CHECKF(err == 0, "the directory vanished across a remount: %d", err);
		if (err) { session_close(&s); goto out; }
		verify_lookup(dir, set, "growth/remounted");
		verify_readdir(dir, set, "growth/remounted");
	}

close:
	if (dir)
		ntfs_inode_put(dir);
	session_close(&s);
	fsck_ok(scratch, "a fully grown directory");
out:
	nameset_free(set);
	cleanup_image(scratch);
	printf("test_multilevel_growth\n");
}

/* ---- 3. deleting back to empty, in three orders ----------------------- */

/*
 * Deleting is where ntfs_index_rm_leaf(), ntfs_ih_takeout() and
 * ntfs_ir_leafify() run. The order matters: creation order always empties the
 * leftmost leaf, reverse order always the rightmost, and a shuffle empties them
 * from the middle outwards. Each drives a different branch of rm_leaf.
 */
static void delete_run(const char *tag, int order)
{
	struct session s;
	ntfs_inode_t *dir = NULL, *f = NULL;
	struct nameset *set;
	uint64_t dir_no = 0;
	const int n = DELETE_ENTRIES;
	int *del;
	int err;

	scratch_path(tag);
	if (copy_base(scratch) != 0) {
		fprintf(stderr, "SKIP delete_run(%s): no base image\n", tag);
		return;
	}
	set = nameset_new(n);
	del = calloc((size_t)n, sizeof(int));
	if (!set || !del) { nameset_free(set); free(del); unlink(scratch); return; }
	for (int i = 0; i < n; i++) {
		snprintf(set->names[i], NAME_MAX_LEN, "victim-%06d", i);
		del[i] = i;
	}
	nameset_seal(set);
	if (order == 1)
		for (int i = 0; i < n / 2; i++) {
			int t = del[i]; del[i] = del[n - 1 - i]; del[n - 1 - i] = t;
		}
	else if (order == 2)
		shuffle(del, n, 0x5eed1234u);

	err = session_open(&s, scratch);
	CHECKF(err == 0, "%s: mount failed: %d", tag, err);
	if (err) goto out;
	err = ntfs_mkdir(s.root, "doomed", 0040755, &dir);
	CHECKF(err == 0, "%s: mkdir failed: %d", tag, err);
	if (err) { session_close(&s); goto out; }
	dir_no = ntfs_inode_number(dir);

	for (int i = 0; i < n; i++) {
		err = ntfs_create(dir, set->names[i], 0100644, &f);
		if (err) {
			CHECKF(false, "%s: create('%s') failed: %d", tag, set->names[i], err);
			goto close;
		}
		ntfs_inode_put(f);
		set->live[i] = true;
		set->nlive++;
	}
	verify_readdir(dir, set, tag);

	for (int k = 0; k < n; k++) {
		int i = del[k];

		err = ntfs_unlink(dir, set->names[i]);
		if (err) {
			CHECKF(false, "%s: unlink('%s') failed after %d removals: %d",
			       tag, set->names[i], k, err);
			goto close;
		}
		set->live[i] = false;
		set->nlive--;

		/* Four sweeps across the run, not one per removal: the sweep is
		 * O(n log n) and the point is to catch the tree going wrong
		 * somewhere in the middle, not to bisect it. */
		if (((k + 1) % (n / 4)) == 0 && k + 1 < n) {
			verify_lookup(dir, set, tag);
			verify_readdir(dir, set, tag);
		}
	}

	CHECKF(set->nlive == 0, "%s: bookkeeping error", tag);
	verify_readdir(dir, set, tag);		/* must now be just . and .. */
	verify_lookup(dir, set, tag);		/* and nothing may still look up */

	ntfs_inode_put(dir);
	dir = NULL;
close:
	if (dir)
		ntfs_inode_put(dir);
	session_close(&s);
	fsck_ok(scratch, tag);

	{
		struct idx_shape sh;

		if (read_shape(scratch, dir_no, &sh) == 0) {
			/* Emptying the directory collapses the tree back into
			 * the resident index root (ntfs_ir_leafify clears
			 * LARGE_INDEX). The $INDEX_ALLOCATION attribute itself
			 * stays behind, empty: that is current behaviour, not
			 * an assertion that it is the only correct one. */
			CHECKF(!sh.large_index,
			       "%s: the index did not collapse back into $INDEX_ROOT", tag);
			CHECKF(sh.height == 1, "%s: an emptied directory is still %d deep",
			       tag, sh.height);
		}
	}

	/* The directory must still be usable afterwards, not just empty. */
	if (session_open(&s, scratch) == 0) {
		if (ntfs_lookup(s.root, "doomed", &dir) == 0) {
			ntfs_inode_t *again = NULL;

			verify_readdir(dir, set, tag);
			err = ntfs_create(dir, "after-the-purge", 0100644, &again);
			CHECKF(err == 0, "%s: could not create in the emptied directory: %d",
			       tag, err);
			if (!err)
				ntfs_inode_put(again);
			ntfs_inode_put(dir);
		} else {
			CHECKF(false, "%s: the emptied directory vanished", tag);
		}
		session_close(&s);
		fsck_ok(scratch, "reusing an emptied directory");
	}
out:
	nameset_free(set);
	free(del);
	cleanup_image(scratch);
}

static void test_delete_orders(void)
{
	delete_run("delete-creation-order", 0);
	delete_run("delete-reverse-order", 1);
	delete_run("delete-shuffled", 2);
	printf("test_delete_orders\n");
}

/* ---- 4. deleting out of the middle ------------------------------------ */

/*
 * Emptying a leaf that has live neighbours on both sides is the case that makes
 * a parent hand an entry down: ntfs_index_rm_leaf() finds a non-END entry in
 * the parent and calls ntfs_ih_takeout(), which deletes it there and re-adds it
 * lower down. Deleting from either end never reaches that branch, which is why
 * it gets its own test.
 */
static void test_delete_middle(void)
{
	struct session s;
	ntfs_inode_t *dir = NULL, *f = NULL;
	struct nameset *set;
	const int n = MIDDLE_ENTRIES;
	const int lo = n / 3, hi = (2 * n) / 3;
	int err;

	scratch_path("middle");
	if (copy_base(scratch) != 0) {
		fprintf(stderr, "SKIP test_delete_middle: no base image\n");
		return;
	}
	set = nameset_new(n);
	if (!set) { unlink(scratch); return; }
	for (int i = 0; i < n; i++)
		snprintf(set->names[i], NAME_MAX_LEN, "mid-%06d", i);
	nameset_seal(set);

	err = session_open(&s, scratch);
	CHECKF(err == 0, "mount failed: %d", err);
	if (err) goto out;
	err = ntfs_mkdir(s.root, "middle", 0040755, &dir);
	CHECKF(err == 0, "mkdir failed: %d", err);
	if (err) { session_close(&s); goto out; }

	for (int i = 0; i < n; i++) {
		err = ntfs_create(dir, set->names[i], 0100644, &f);
		if (err) {
			CHECKF(false, "create('%s') failed: %d", set->names[i], err);
			goto close;
		}
		ntfs_inode_put(f);
		set->live[i] = true;
		set->nlive++;
	}

	/* The names sort in the same order as their numbers, so [lo, hi) is a
	 * contiguous stretch of the key space: whole leaves empty while their
	 * neighbours stay populated. */
	for (int i = lo; i < hi; i++) {
		err = ntfs_unlink(dir, set->names[i]);
		if (err) {
			CHECKF(false, "unlink('%s') failed: %d", set->names[i], err);
			goto close;
		}
		set->live[i] = false;
		set->nlive--;
	}
	verify_lookup(dir, set, "middle/mounted");
	verify_readdir(dir, set, "middle/mounted");

	ntfs_inode_put(dir);
	dir = NULL;
	session_close(&s);
	fsck_ok(scratch, "deleting the middle third");

	/* Put the middle back. Re-inserting into the gap the removals left is
	 * how an index that "worked" but left a stale key in a parent gives
	 * itself away. */
	err = session_open(&s, scratch);
	CHECKF(err == 0, "remount failed: %d", err);
	if (err) goto out;
	err = ntfs_lookup(s.root, "middle", &dir);
	CHECKF(err == 0, "the directory vanished: %d", err);
	if (err) { session_close(&s); goto out; }
	verify_lookup(dir, set, "middle/remounted");
	verify_readdir(dir, set, "middle/remounted");

	for (int i = lo; i < hi; i++) {
		err = ntfs_create(dir, set->names[i], 0100644, &f);
		if (err) {
			CHECKF(false, "re-create('%s') failed: %d", set->names[i], err);
			goto close;
		}
		ntfs_inode_put(f);
		set->live[i] = true;
		set->nlive++;
	}
	verify_lookup(dir, set, "middle/refilled");
	verify_readdir(dir, set, "middle/refilled");

	ntfs_inode_put(dir);
	dir = NULL;
close:
	if (dir)
		ntfs_inode_put(dir);
	session_close(&s);
	fsck_ok(scratch, "refilling the middle third");
out:
	nameset_free(set);
	cleanup_image(scratch);
	printf("test_delete_middle\n");
}

/* ---- 5. key shapes: adjacent and far apart ---------------------------- */

/*
 * ntfs_ie_get_median() picks the split point by position, but which keys end up
 * on each side, and how long the promoted key is, is decided by collation. Two
 * shapes are worth separating:
 *
 *   adjacent  one long shared prefix, so every key is close to every other and
 *             the promoted median is a long key -- the expensive case for
 *             ntfs_ir_insert_median(), which has to fit it into the index root.
 *   spread    keys scattered across the collation order, so inserts land in
 *             every leaf rather than marching rightwards.
 *
 * Crossed with three insertion orders, because ascending insertion only ever
 * splits the rightmost node while a shuffle splits everywhere.
 */
static void collate_run(const char *tag, bool adjacent, int order)
{
	static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	struct session s;
	ntfs_inode_t *dir = NULL, *f = NULL;
	struct nameset *set;
	const int n = COLLATE_ENTRIES;
	int *ins;
	int err;

	scratch_path(tag);
	if (copy_base(scratch) != 0) {
		fprintf(stderr, "SKIP collate_run(%s): no base image\n", tag);
		return;
	}
	set = nameset_new(n);
	ins = calloc((size_t)n, sizeof(int));
	if (!set || !ins) { nameset_free(set); free(ins); unlink(scratch); return; }

	for (int i = 0; i < n; i++) {
		if (adjacent)
			/* 26 shared characters, then four that differ. NTFS
			 * collates on the upcased UTF-16 name, so keep the
			 * whole family one case: two names that differ only in
			 * case would collide, not sort. */
			snprintf(set->names[i], NAME_MAX_LEN,
				 "aaaaaaaaaaaaaaaaaaaaaaaaaa%04d", i);
		else
			/* First character sweeps the whole alphabet, so
			 * consecutive inserts land in different leaves. */
			snprintf(set->names[i], NAME_MAX_LEN, "%c%c-%04d",
				 alphabet[i % 36], alphabet[(i / 36) % 36], i);
		ins[i] = i;
	}
	nameset_seal(set);
	if (order == 1)
		for (int i = 0; i < n / 2; i++) {
			int t = ins[i]; ins[i] = ins[n - 1 - i]; ins[n - 1 - i] = t;
		}
	else if (order == 2)
		shuffle(ins, n, 0xc0ffee01u);

	err = session_open(&s, scratch);
	CHECKF(err == 0, "%s: mount failed: %d", tag, err);
	if (err) goto out;
	err = ntfs_mkdir(s.root, "keys", 0040755, &dir);
	CHECKF(err == 0, "%s: mkdir failed: %d", tag, err);
	if (err) { session_close(&s); goto out; }

	for (int k = 0; k < n; k++) {
		int i = ins[k];

		err = ntfs_create(dir, set->names[i], 0100644, &f);
		if (err) {
			CHECKF(false, "%s: create('%s') failed: %d", tag, set->names[i], err);
			goto close;
		}
		ntfs_inode_put(f);
		set->live[i] = true;
		set->nlive++;
	}
	verify_lookup(dir, set, tag);
	verify_readdir(dir, set, tag);

	/* Take half of them out again, in the same order they went in, so the
	 * removals are as scattered (or as clustered) as the inserts were. */
	for (int k = 0; k < n; k += 2) {
		int i = ins[k];

		err = ntfs_unlink(dir, set->names[i]);
		if (err) {
			CHECKF(false, "%s: unlink('%s') failed: %d", tag, set->names[i], err);
			goto close;
		}
		set->live[i] = false;
		set->nlive--;
	}
	verify_lookup(dir, set, tag);
	verify_readdir(dir, set, tag);

	ntfs_inode_put(dir);
	dir = NULL;
	session_close(&s);
	fsck_ok(scratch, tag);

	if (session_open(&s, scratch) == 0) {
		if (ntfs_lookup(s.root, "keys", &dir) == 0) {
			verify_lookup(dir, set, tag);
			verify_readdir(dir, set, tag);
			ntfs_inode_put(dir);
			dir = NULL;
		} else {
			CHECKF(false, "%s: the directory vanished", tag);
		}
	}
close:
	if (dir)
		ntfs_inode_put(dir);
	session_close(&s);
out:
	nameset_free(set);
	free(ins);
	cleanup_image(scratch);
}

static void test_collation_shapes(void)
{
	collate_run("adjacent-ascending", true, 0);
	collate_run("adjacent-descending", true, 1);
	collate_run("adjacent-shuffled", true, 2);
	collate_run("spread-ascending", false, 0);
	collate_run("spread-shuffled", false, 2);
	printf("test_collation_shapes\n");
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	const char *mk = tool_path("NTFS_MKNTFS", "tools/.local/sbin/mkntfs");

	if (!have_tool(mk)) {
		fprintf(stderr, "SKIP test_bigdir: no mkntfs at %s "
			"(build it with tools/build-ntfsprogs.sh)\n", mk);
		return 0;
	}
	if (!fsck_available())
		fprintf(stderr, "WARNING test_bigdir: no ntfsck at %s -- the "
			"structural checks, which are the valuable half of this "
			"file, will not run (tools/build-ntfsprogs-plus.sh)\n",
			tool_path("NTFS_NTFSCK", "tools/.local-plus/sbin/ntfsck"));

	snprintf(base_img, sizeof(base_img), "/tmp/ttntfs-bigdir-%d-base.img", (int)getpid());
	if (make_image(base_img, 64) != 0) {
		fprintf(stderr, "SKIP test_bigdir: mkntfs failed on %s\n", base_img);
		return 0;
	}
	if (!fsck_ok(base_img, "mkntfs")) {
		unlink(base_img);
		return 1;		/* nothing below means anything */
	}

	test_index_root_boundary();
	test_multilevel_growth();
	test_delete_orders();
	test_delete_middle();
	test_collation_shapes();

	cleanup_image(base_img);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
