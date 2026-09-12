/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mkfixtures - generate NTFS test images with libntfs-3g and write a JSON
 * manifest describing every file on them (path, size, sha256, streams,
 * flags, timestamps) so an independent implementation (tools/ntfscli against
 * the ported kernel driver) can be checked against them.
 *
 * Every fixture is: mkntfs -F -Q -c <cluster> [-L label] <img>, then
 * ntfs_mount() and a populate function that uses the libntfs-3g inode API.
 * The manifest is not derived from what we *intended* to write: after
 * populating, the image is unmounted, re-mounted and walked with
 * ntfs_readdir/ntfs_attr_pread, so it records what libntfs-3g itself reads
 * back. Adding a fixture = adding one function and one line in fixtures[].
 *
 * Content is deterministic: every byte comes from a xorshift64* generator
 * seeded from the path (and stream name), so images are reproducible modulo
 * timestamps and allocation choices.
 *
 * libntfs-3g quirks worked around here (see also tools/README.md):
 *  - ntfs_set_ntfs_attrib() refuses FILE_ATTR_COMPRESSED on regular files
 *    (FILE_ATTR_SETTABLE only adds it for directories). A file becomes
 *    compressed when ni->flags has FILE_ATTR_COMPRESSED *before* the first
 *    ntfs_attr_open() of its empty $DATA (that is how ntfs_create inherits
 *    compression from a compressed parent). We set the flag directly.
 *  - Compression is only honoured if NVolCompression(vol) is set (default
 *    off in the library; the FUSE driver sets it from a mount option) and
 *    cluster size <= 4 KiB.
 *  - Sparse files: no explicit flag. ntfs_attr_truncate() beyond EOF and
 *    pwrite past EOF allocate holes (HOLES_OK) and the library sets
 *    ATTR_IS_SPARSE / FILE_ATTR_SPARSE_FILE itself.
 *  - Timestamps: ntfs_inode_set_times() takes {create, modify, access} as
 *    NTFS 100 ns times; it sets change time to "now" and marks TimesSet so
 *    closing the inode does not overwrite them. Set them last.
 *  - $ATTRIBUTE_LIST: ntfs_attr_add()/ntfs_attr_update_mapping_pairs() add
 *    one automatically when the base record fills up.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ntfs-3g/types.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/security.h>

#include "../common/json.h"
#include "../common/sha256.h"

#define KiB 1024ULL
#define MiB (1024ULL * KiB)

static const char *g_outdir = "images";
static const char *g_mkntfs = "mkntfs";
static uint64_t g_seed = 0x5eed1e55ULL;
static bool g_force;
static bool g_verbose;

/* ------------------------------------------------------------------------ */
/* diagnostics                                                              */

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("mkfixtures: ", stderr);
	vfprintf(stderr, fmt, ap);
	if (errno)
		fprintf(stderr, ": %s", strerror(errno));
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------------ */
/* deterministic content                                                    */

enum fill { FILL_RANDOM, FILL_TEXT, FILL_ZERO };

static uint64_t fnv1a(const char *s, uint64_t h)
{
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static uint64_t seed_for(const char *path, const char *stream)
{
	uint64_t h = fnv1a(path, 0xcbf29ce484222325ULL ^ g_seed);
	if (stream)
		h = fnv1a(stream, h ^ 0x9e3779b97f4a7c15ULL);
	return h ? h : 1;
}

static uint64_t xs64(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/* Fill @buf as if it were bytes [@off, @off+len) of a stream whose generator
 * state at @off is *@state. For FILL_TEXT the content is a repeating,
 * highly compressible pseudo-sentence pattern with a small random element.*/
static void fill_buf(uint8_t *buf, size_t len, uint64_t *state, enum fill kind, uint64_t off)
{
	static const char words[] = "the quick brown fox jumps over the lazy dog "
		"NTFS fixture text line that compresses very well indeed ";
	size_t i;

	switch (kind) {
	case FILL_ZERO:
		memset(buf, 0, len);
		return;
	case FILL_TEXT:
		for (i = 0; i < len; i++) {
			uint64_t pos = off + i;
			if (pos % 4096 == 0)
				buf[i] = (uint8_t)('A' + (xs64(state) % 26));
			else
				buf[i] = (uint8_t)words[pos % (sizeof(words) - 1)];
		}
		return;
	case FILL_RANDOM:
	default:
		for (i = 0; i + 8 <= len; i += 8) {
			uint64_t v = xs64(state);
			memcpy(buf + i, &v, 8);
		}
		if (i < len) {
			uint64_t v = xs64(state);
			memcpy(buf + i, &v, len - i);
		}
		return;
	}
}

/* ------------------------------------------------------------------------ */
/* fixture context                                                          */

struct fx {
	const char *name;
	char img[1024];
	char manifest[1024];
	uint64_t size;
	uint32_t cluster;
	const char *label;
	ntfs_volume *vol;
	uint64_t files, dirs, bytes;
};

static int ucs(const char *utf8, ntfschar **out)
{
	int len = ntfs_mbstoucs(utf8, out);
	if (len < 0)
		die("ntfs_mbstoucs(%s)", utf8);
	if (len > 255)
		die("name too long (%d UTF-16 units): %s", len, utf8);
	return len;
}

static void split_path(const char *path, char *dir, size_t dirsz, const char **base)
{
	const char *slash = strrchr(path, '/');
	if (!slash)
		die("path must be absolute: %s", path);
	size_t n = slash - path;
	if (n == 0)
		n = 1; /* root */
	if (n + 1 > dirsz)
		die("path too long: %s", path);
	memcpy(dir, path, n);
	dir[n] = 0;
	*base = slash + 1;
}

static ntfs_inode *fx_open(struct fx *fx, const char *path)
{
	ntfs_inode *ni = ntfs_pathname_to_inode(fx->vol, NULL, path);
	if (!ni)
		die("lookup %s", path);
	return ni;
}

static void fx_close(ntfs_inode *ni)
{
	if (ni && ntfs_inode_close(ni))
		die("ntfs_inode_close(%" PRIu64 ")", (uint64_t)ni->mft_no);
}

/* Close a child while its parent directory is still open. Plain
 * ntfs_inode_close() re-opens the parent from disk to sync $FILE_NAME and
 * would not see the parent's dirty (unwritten) index root. */
static void fx_close_in(ntfs_inode *ni, ntfs_inode *dir)
{
	if (ntfs_inode_close_in_dir(ni, dir))
		die("ntfs_inode_close_in_dir(%" PRIu64 ")", (uint64_t)ni->mft_no);
}

/* Create a regular file or directory named @name in @dir. */
static ntfs_inode *fx_create_in(struct fx *fx, ntfs_inode *dir, const char *name, mode_t type)
{
	ntfschar *uname = NULL;
	int ulen = ucs(name, &uname);
	ntfs_inode *ni = ntfs_create(dir, const_cpu_to_le32(0), uname, (u8)ulen, type);
	free(uname);
	if (!ni)
		die("ntfs_create(%s)", name);
	if (S_ISDIR(type))
		fx->dirs++;
	else
		fx->files++;
	return ni;
}

static ntfs_inode *fx_create(struct fx *fx, const char *path, mode_t type)
{
	char dpath[1024];
	const char *base;
	split_path(path, dpath, sizeof(dpath), &base);
	ntfs_inode *dir = fx_open(fx, dpath);
	ntfs_inode *ni = fx_create_in(fx, dir, base, type);
	fx_close(dir);
	return ni;
}

static void fx_mkdir(struct fx *fx, const char *path)
{
	fx_close(fx_create(fx, path, S_IFDIR));
}

/* Mark an inode compressed. Must happen before its $DATA is first opened. */
static void fx_mark_compressed(ntfs_inode *ni)
{
	ni->flags |= FILE_ATTR_COMPRESSED;
	NInoSetDirty(ni);
	NInoFileNameSetDirty(ni);
}

/* For a directory: mark compressed the proper way (also flags $INDEX_ROOT),
 * so files created inside inherit compression like on Windows. */
static void fx_dir_set_compressed(ntfs_inode *dir)
{
	le32 attrib = dir->flags | FILE_ATTR_COMPRESSED;
	if (ntfs_set_ntfs_attrib(dir, (const char *)&attrib, sizeof(attrib), 0))
		die("ntfs_set_ntfs_attrib(compressed dir)");
}

/* Write @len generated bytes at @off into stream @stream (NULL = unnamed). */
static void fx_write_stream(struct fx *fx, ntfs_inode *ni, const char *seedpath,
			    const char *stream, uint64_t off, uint64_t len, enum fill kind)
{
	ntfschar *uname = AT_UNNAMED, *alloc = NULL;
	int ulen = 0;
	if (stream) {
		/* ntfs_mbstoucs() needs *out == NULL to allocate; a non-NULL
		 * pointer is treated as a caller buffer (it would scribble over
		 * AT_UNNAMED). */
		ulen = ucs(stream, &alloc);
		uname = alloc;
		/* Create the named stream (resident, empty) if it does not exist. */
		if (!ntfs_attr_exist(ni, AT_DATA, uname, ulen) &&
		    ntfs_attr_add(ni, AT_DATA, uname, (u8)ulen, NULL, 0))
			die("ntfs_attr_add(%s)", stream);
	}
	/* ntfs_attr_open() takes ownership of a non-constant name and frees it
	 * in ntfs_attr_close(); do not free @alloc ourselves. */
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, uname, ulen);
	if (!na)
		die("ntfs_attr_open(%s:%s)", seedpath, stream ? stream : "");
	if (len) {
		static uint8_t *buf;
		const size_t chunk = 1 * MiB;
		if (!buf && !(buf = malloc(chunk)))
			die("malloc");
		uint64_t state = seed_for(seedpath, stream);
		/* advance the generator to @off so content is a pure function of offset */
		for (uint64_t i = 0; i < off / 8; i++)
			xs64(&state);
		uint64_t pos = off, end = off + len;
		while (pos < end) {
			size_t n = end - pos < chunk ? (size_t)(end - pos) : chunk;
			fill_buf(buf, n, &state, kind, pos);
			s64 w = ntfs_attr_pwrite(na, (s64)pos, (s64)n, buf);
			if (w != (s64)n)
				die("ntfs_attr_pwrite(%s:%s, off=%" PRIu64 ", n=%zu) -> %lld",
				    seedpath, stream ? stream : "", pos, n, (long long)w);
			pos += n;
		}
		fx->bytes += len;
	}
	ntfs_attr_close(na);
}

/* Set the unnamed stream's size (extends with holes / truncates). */
static void fx_truncate(ntfs_inode *ni, uint64_t size)
{
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na)
		die("ntfs_attr_open for truncate");
	if (ntfs_attr_truncate(na, (s64)size))
		die("ntfs_attr_truncate(%" PRIu64 ")", size);
	ntfs_attr_close(na);
}

enum { FXF_COMPRESSED = 1 };

/* Create a whole file with generated content in one call. */
static ntfs_inode *fx_file_open(struct fx *fx, const char *path, uint64_t size, enum fill kind, int flags)
{
	ntfs_inode *ni = fx_create(fx, path, S_IFREG);
	if (flags & FXF_COMPRESSED)
		fx_mark_compressed(ni);
	fx_write_stream(fx, ni, path, NULL, 0, size, kind);
	return ni;
}

static void fx_file(struct fx *fx, const char *path, uint64_t size, enum fill kind, int flags)
{
	fx_close(fx_file_open(fx, path, size, kind, flags));
}

static void fx_link(struct fx *fx, const char *existing, const char *newpath)
{
	char dpath[1024];
	const char *base;
	split_path(newpath, dpath, sizeof(dpath), &base);
	ntfs_inode *ni = fx_open(fx, existing);
	ntfs_inode *dir = fx_open(fx, dpath);
	ntfschar *uname = NULL;
	int ulen = ucs(base, &uname);
	if (ntfs_link(ni, dir, uname, (u8)ulen))
		die("ntfs_link(%s -> %s)", existing, newpath);
	free(uname);
	fx_close(dir);
	fx_close(ni);
}

/* Unix seconds (+ns) -> NTFS 100 ns intervals since 1601. */
static u64 unix_to_ntfs(int64_t sec, int32_t nsec)
{
	return (u64)((sec + (int64_t)NTFS_TIME_OFFSET / 10000000LL) * 10000000LL + nsec / 100);
}

/* Explicit create / modify / access times; change time becomes "now". */
static void fx_set_times(ntfs_inode *ni, int64_t crtime, int64_t mtime, int32_t mtime_ns, int64_t atime)
{
	u64 t[3];
	t[0] = unix_to_ntfs(crtime, 0);
	t[1] = unix_to_ntfs(mtime, mtime_ns);
	t[2] = unix_to_ntfs(atime, 0);
	if (ntfs_inode_set_times(ni, (const char *)t, sizeof(t), 0))
		die("ntfs_inode_set_times");
}

/* ------------------------------------------------------------------------ */
/* volume lifecycle                                                         */

static int run(const char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int nul = open("/dev/null", O_WRONLY);
		if (!g_verbose && nul >= 0)
			dup2(nul, 1), dup2(nul, 2);
		execvp(argv[0], (char *const *)argv);
		perror(argv[0]);
		_exit(127);
	}
	int st;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void fx_begin(struct fx *fx)
{
	char csz[32], lbl[64];
	snprintf(fx->img, sizeof(fx->img), "%s/%s.img", g_outdir, fx->name);
	snprintf(fx->manifest, sizeof(fx->manifest), "%s/%s.manifest.json", g_outdir, fx->name);
	snprintf(csz, sizeof(csz), "%u", fx->cluster);
	snprintf(lbl, sizeof(lbl), "%s", fx->label ? fx->label : fx->name);

	unlink(fx->img);
	int fd = open(fx->img, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, (off_t)fx->size))
		die("create %s", fx->img);
	close(fd);

	const char *argv[] = { g_mkntfs, "-F", "-Q", "-q", "-c", csz, "-L", lbl, fx->img, NULL };
	if (run(argv))
		die("%s failed for %s", g_mkntfs, fx->img);

	/* mkntfs derives the serial from the clock, so images made in the same
	 * second collide. Make it a function of the fixture name instead. */
	char serial[64], labelbin[1024];
	snprintf(serial, sizeof(serial), "--new-serial=%016" PRIx64, fnv1a(fx->name, 0x243f6a8885a308d3ULL));
	const char *slash = strrchr(g_mkntfs, '/');
	if (slash)
		snprintf(labelbin, sizeof(labelbin), "%.*s/ntfslabel", (int)(slash - g_mkntfs), g_mkntfs);
	else
		snprintf(labelbin, sizeof(labelbin), "ntfslabel");
	const char *argv2[] = { labelbin, serial, fx->img, NULL };
	if (run(argv2))
		die("%s failed for %s", labelbin, fx->img);

	fx->vol = ntfs_mount(fx->img, NTFS_MNT_NONE);
	if (!fx->vol)
		die("ntfs_mount(%s)", fx->img);
	NVolSetCompression(fx->vol);	/* allow compressed streams (library default: off) */
	fx->files = fx->dirs = fx->bytes = 0;
}

static void fx_unmount(struct fx *fx)
{
	if (fx->vol && ntfs_umount(fx->vol, FALSE))
		die("ntfs_umount(%s)", fx->img);
	fx->vol = NULL;
}

/* ------------------------------------------------------------------------ */
/* manifest: re-mount read-only and walk                                    */

struct dent {
	char *name;
	u64 mref;
	unsigned dt;
};
struct dlist { struct dent *v; size_t n, cap; };

static int filldir(void *ctx, const ntfschar *name, const int name_len, const int name_type,
		   const s64 pos, const MFT_REF mref, const unsigned dt_type)
{
	struct dlist *l = ctx;
	(void)pos;
	if (name_type == FILE_NAME_DOS)
		return 0;
	if (MREF(mref) < FILE_first_user)
		return 0;		/* system files, "." and ".." (root is 5) */
	char *mbs = NULL;
	if (ntfs_ucstombs(name, name_len, &mbs, 0) < 0)
		die("ntfs_ucstombs");
	if (!strcmp(mbs, ".") || !strcmp(mbs, "..")) {
		free(mbs);
		return 0;
	}
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		l->v = realloc(l->v, l->cap * sizeof(*l->v));
		if (!l->v)
			die("realloc");
	}
	l->v[l->n++] = (struct dent){ mbs, mref, dt_type };
	return 0;
}

static int dent_cmp(const void *a, const void *b)
{
	return strcmp(((const struct dent *)a)->name, ((const struct dent *)b)->name);
}

static void ntfs_time_iso(ntfs_time t, char out[40])
{
	struct timespec ts = ntfs2timespec(t);
	struct tm tm;
	time_t sec = ts.tv_sec;
	gmtime_r(&sec, &tm);
	snprintf(out, 40, "%04d-%02d-%02dT%02d:%02d:%02d.%07ldZ",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
		 ts.tv_nsec / 100);
}

/* sha256 of a whole stream via ntfs_attr_pread; returns data size. */
static s64 hash_stream(ntfs_inode *ni, ntfschar *uname, u32 ulen, char hex[65], const char *what)
{
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, uname, ulen);
	if (!na)
		die("manifest: ntfs_attr_open(%s)", what);
	static uint8_t *buf;
	const size_t chunk = 1 * MiB;
	if (!buf && !(buf = malloc(chunk)))
		die("malloc");
	struct sha256_ctx c;
	sha256_init(&c);
	s64 size = na->data_size, pos = 0;
	while (pos < size) {
		s64 n = size - pos < (s64)chunk ? size - pos : (s64)chunk;
		s64 r = ntfs_attr_pread(na, pos, n, buf);
		if (r != n)
			die("manifest: ntfs_attr_pread(%s @%lld n=%lld) -> %lld", what,
			    (long long)pos, (long long)n, (long long)r);
		sha256_update(&c, buf, (size_t)n);
		pos += n;
	}
	uint8_t d[32];
	sha256_final(&c, d);
	sha256_hex(d, hex);
	ntfs_attr_close(na);
	return size;
}

static void emit_entry(FILE *mf, ntfs_volume *vol, u64 mref, const char *path, bool *first)
{
	ntfs_inode *ni = ntfs_inode_open(vol, MREF(mref));
	if (!ni)
		die("manifest: ntfs_inode_open(%s)", path);
	bool isdir = ni->mrec->flags & MFT_RECORD_IS_DIRECTORY;
	bool reparse = ni->flags & FILE_ATTR_REPARSE_POINT;
	char hex[65], mtime[40], crtime[40];
	s64 size = 0;

	if (!isdir)
		size = hash_stream(ni, AT_UNNAMED, 0, hex, path);
	ntfs_time_iso(ni->last_data_change_time, mtime);
	ntfs_time_iso(ni->creation_time, crtime);

	fprintf(mf, "%s\n    {\"path\": ", *first ? "" : ",");
	*first = false;
	json_write_string(mf, path);
	fprintf(mf, ", \"type\": \"%s\", \"mft_no\": %" PRIu64 ", \"size\": %lld, \"sha256\": ",
		isdir ? "dir" : reparse ? "other" : "file", (uint64_t)MREF(mref), (long long)size);
	if (isdir)
		fputs("null", mf);
	else
		fprintf(mf, "\"%s\"", hex);
	fprintf(mf, ",\n     \"nlink\": %u, \"compressed\": %s, \"sparse\": %s, \"file_attributes\": %u,",
		le16_to_cpu(ni->mrec->link_count),
		(ni->flags & FILE_ATTR_COMPRESSED) ? "true" : "false",
		(ni->flags & FILE_ATTR_SPARSE_FILE) ? "true" : "false",
		le32_to_cpu(ni->flags));
	fprintf(mf, " \"mtime\": \"%s\", \"crtime\": \"%s\",\n     \"streams\": [", mtime, crtime);

	/* named $DATA streams (skip extents: lowest_vcn != 0) */
	ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
	if (!ctx)
		die("manifest: search ctx");
	bool sfirst = true;
	while (!ntfs_attr_lookup(AT_DATA, NULL, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
		ATTR_RECORD *a = ctx->attr;
		if (!a->name_length)
			continue;
		if (a->non_resident && a->lowest_vcn)
			continue;
		ntfschar *uname = (ntfschar *)((u8 *)a + le16_to_cpu(a->name_offset));
		u32 ulen = a->name_length;
		ntfschar *ucopy = ntfs_ucsndup(uname, ulen);
		char *sname = NULL;
		if (!ucopy || ntfs_ucstombs(uname, ulen, &sname, 0) < 0)
			die("manifest: stream name");
		char shex[65];
		s64 ssize = hash_stream(ni, ucopy, ulen, shex, sname);
		fprintf(mf, "%s{\"name\": ", sfirst ? "" : ", ");
		json_write_string(mf, sname);
		fprintf(mf, ", \"size\": %lld, \"sha256\": \"%s\"}", (long long)ssize, shex);
		sfirst = false;
		free(sname);	/* ucopy was freed by ntfs_attr_close() */
	}
	ntfs_attr_put_search_ctx(ctx);
	fputs("]}", mf);
	fx_close(ni);
}

static void walk(FILE *mf, ntfs_volume *vol, u64 dir_mref, const char *path, bool *first, uint64_t *count)
{
	ntfs_inode *dir = ntfs_inode_open(vol, MREF(dir_mref));
	if (!dir)
		die("manifest: open dir %s", path);
	struct dlist l = { 0 };
	s64 pos = 0;
	if (ntfs_readdir(dir, &pos, &l, filldir))
		die("manifest: ntfs_readdir(%s)", path);
	fx_close(dir);
	qsort(l.v, l.n, sizeof(*l.v), dent_cmp);
	for (size_t i = 0; i < l.n; i++) {
		char child[4096];
		snprintf(child, sizeof(child), "%s/%s", strcmp(path, "/") ? path : "", l.v[i].name);
		emit_entry(mf, vol, l.v[i].mref, child, first);
		(*count)++;
		if (l.v[i].dt == NTFS_DT_DIR)
			walk(mf, vol, l.v[i].mref, child, first, count);
		free(l.v[i].name);
	}
	free(l.v);
}

static uint64_t read_serial(const char *img)
{
	int fd = open(img, O_RDONLY);
	uint8_t bs[512];
	if (fd < 0 || pread(fd, bs, sizeof(bs), 0) != (ssize_t)sizeof(bs))
		die("read boot sector of %s", img);
	close(fd);
	uint64_t s;
	memcpy(&s, bs + 0x48, 8);	/* NTFS_BOOT_SECTOR.volume_serial_number */
	return s;
}

static void fx_write_manifest(struct fx *fx)
{
	ntfs_volume *vol = ntfs_mount(fx->img, NTFS_MNT_RDONLY);
	if (!vol)
		die("re-mount %s", fx->img);
	FILE *mf = fopen(fx->manifest, "w");
	if (!mf)
		die("open %s", fx->manifest);
	char *label = vol->vol_name ? vol->vol_name : "";
	fprintf(mf, "{\n  \"image\": ");
	json_write_string(mf, strrchr(fx->img, '/') ? strrchr(fx->img, '/') + 1 : fx->img);
	fprintf(mf, ",\n  \"generator\": \"mkfixtures/libntfs-3g %s\",\n", "2026.7.7");
	fprintf(mf, "  \"seed\": %" PRIu64 ",\n", g_seed);
	fprintf(mf, "  \"volume\": {\"label\": ");
	json_write_string(mf, label);
	fprintf(mf, ", \"serial\": %" PRIu64 ", \"serial_hex\": \"%016" PRIx64 "\", \"version\": \"%u.%u\",\n"
		    "             \"cluster_size\": %u, \"sector_size\": %u, \"mft_record_size\": %u, "
		    "\"index_record_size\": %u, \"total_clusters\": %lld, \"image_size\": %" PRIu64 "},\n",
		read_serial(fx->img), read_serial(fx->img), vol->major_ver, vol->minor_ver,
		vol->cluster_size, vol->sector_size, vol->mft_record_size, vol->indx_record_size,
		(long long)vol->nr_clusters, fx->size);
	fprintf(mf, "  \"entries\": [");
	bool first = true;
	uint64_t count = 0;
	walk(mf, vol, FILE_root, "/", &first, &count);
	fprintf(mf, "\n  ],\n  \"entry_count\": %" PRIu64 "\n}\n", count);
	fclose(mf);
	if (ntfs_umount(vol, FALSE))
		die("umount after manifest");
}

/* ------------------------------------------------------------------------ */
/* fixtures                                                                 */

/* Shared "basic" content: nested dirs, ~200 files of the interesting sizes,
 * hard links, three ADS on one file, explicit timestamps. @big adds the
 * 20 MiB files (skipped for the extreme-cluster images to keep them small).*/
static void populate_basic(struct fx *fx, bool big)
{
	static const struct { uint64_t size; int count; } sizes[] = {
		{ 0, 30 }, { 1, 30 }, { 511, 30 }, { 512, 30 }, { 4095, 25 },
		{ 4096, 25 }, { 65536, 20 }, { 1 * MiB, 8 }, { 20 * MiB, 2 },
	};
	static const char *dirs[] = {
		"/files", "/files/sz", "/nested", "/nested/a", "/nested/a/b", "/nested/a/b/c",
		"/nested/a/b/c/d", "/links", "/ads", "/times", "/empty-dir",
	};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
		fx_mkdir(fx, dirs[i]);

	/* size classes, spread over sub-dirs so directories have >1 index block */
	int n = 0;
	for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); s++) {
		if (sizes[s].size >= 20 * MiB && !big)
			continue;
		char dir[64];
		snprintf(dir, sizeof(dir), "/files/sz/%" PRIu64, sizes[s].size);
		fx_mkdir(fx, dir);
		ntfs_inode *dni = fx_open(fx, dir);
		for (int i = 0; i < sizes[s].count; i++, n++) {
			char name[64], path[128];
			snprintf(name, sizeof(name), "f%03d_%" PRIu64 ".bin", n, sizes[s].size);
			snprintf(path, sizeof(path), "%s/%s", dir, name);
			ntfs_inode *ni = fx_create_in(fx, dni, name, S_IFREG);
			/* alternate content types so some are compressible text */
			fx_write_stream(fx, ni, path, NULL, 0, sizes[s].size, (i % 3) ? FILL_RANDOM : FILL_TEXT);
			fx_close_in(ni, dni);
		}
		fx_close(dni);
	}

	/* deep nesting */
	fx_file(fx, "/nested/a/b/c/d/deep.txt", 1234, FILL_TEXT, 0);
	fx_file(fx, "/nested/a/b/mid.bin", 70000, FILL_RANDOM, 0);
	fx_file(fx, "/nested/top.bin", 5000, FILL_RANDOM, 0);

	/* hard links: same inode under three names in two directories */
	fx_file(fx, "/links/original.bin", 100000, FILL_RANDOM, 0);
	fx_link(fx, "/links/original.bin", "/links/hardlink-1.bin");
	fx_link(fx, "/links/original.bin", "/nested/hardlink-2.bin");
	fx_file(fx, "/links/two-names.txt", 33, FILL_TEXT, 0);
	fx_link(fx, "/links/two-names.txt", "/links/two-names-alias.txt");

	/* alternate data streams: a resident one, a non-resident one, a big one */
	{
		ntfs_inode *ni = fx_file_open(fx, "/ads/with-streams.bin", 8000, FILL_RANDOM, 0);
		fx_write_stream(fx, ni, "/ads/with-streams.bin", "small", 0, 100, FILL_TEXT);
		fx_write_stream(fx, ni, "/ads/with-streams.bin", "com.apple.FinderInfo", 0, 32, FILL_RANDOM);
		fx_write_stream(fx, ni, "/ads/with-streams.bin", "big stream", 0, 300000, FILL_RANDOM);
		fx_close(ni);
		/* an empty file whose only content is in a named stream */
		ni = fx_file_open(fx, "/ads/only-stream.bin", 0, FILL_RANDOM, 0);
		fx_write_stream(fx, ni, "/ads/only-stream.bin", "Zone.Identifier", 0, 60, FILL_TEXT);
		fx_close(ni);
	}

	/* explicit timestamps (Unix seconds, UTC) */
	{
		ntfs_inode *ni = fx_file_open(fx, "/times/y2001.txt", 10, FILL_TEXT, 0);
		fx_set_times(ni, 1000000000, 1000000000, 0, 1000000000);	/* 2001-09-09T01:46:40Z */
		fx_close(ni);
		ni = fx_file_open(fx, "/times/precise.txt", 20, FILL_TEXT, 0);
		fx_set_times(ni, 1234567890, 1234567890, 123456700, 1234567890);	/* 100 ns precision */
		fx_close(ni);
		ni = fx_file_open(fx, "/times/pre-1970.txt", 30, FILL_TEXT, 0);
		fx_set_times(ni, -315619200, -315619200, 0, -315619200);	/* 1960-01-01T00:00:00Z */
		fx_close(ni);
		ni = fx_file_open(fx, "/times/post-2038.txt", 40, FILL_TEXT, 0);
		fx_set_times(ni, 2147483648LL, 4102444800LL, 5000000, 2147483648LL); /* 2038-01-19 / 2100-01-01 */
		fx_close(ni);
		ni = fx_open(fx, "/times");
		fx_set_times(ni, 1600000000, 1600000000, 0, 1600000000);	/* directory too */
		fx_close(ni);
	}
}

static void fixture_basic_4k(struct fx *fx) { populate_basic(fx, true); }
static void fixture_cluster_512(struct fx *fx) { populate_basic(fx, false); }
static void fixture_cluster_64k(struct fx *fx) { populate_basic(fx, false); }

static void fixture_names(struct fx *fx)
{
	static const char *names[] = {
		"plain.txt",
		"with spaces in name.txt",
		" leading-space.txt",
		"dots.in.the.name.tar.gz",
		".hidden-dotfile",
		"..two-leading-dots",
		"UPPER.TXT", "upper.txt", "Upper.Txt",	/* case-distinct; only equal case-insensitively */
		"日本語ファイル.txt",			/* CJK */
		"中文文件名.md",
		"한국어.txt",
		"emoji-😀🎉.txt",				/* surrogate pairs */
		"family-👨‍👩‍👧‍👦.txt",			/* ZWJ sequence */
		"café-nfc-\xc3\xa9.txt",			/* U+00E9 precomposed */
		"cafe\xcc\x81-nfd-e\xcc\x81.txt",		/* e + U+0301 combining */
		"a\xcc\x8a\xcc\xa8-combining-stack.txt",	/* a + ring + ogonek */
		"Ａｂｃ-fullwidth.txt",
		"ελληνικά-greek.txt",
		"русский-cyrillic.txt",
		"עברית-hebrew.txt",
		"العربية-arabic.txt",
		"tab\there.txt",
		"semi;colon,comma=equals+plus.txt",
		"~tilde#hash%percent&amp@at!bang.txt",
		"[brackets]{braces}(parens).txt",
		"'single'quotes'.txt",
		"$dollar.txt",
		"nul-free-but-c1-\xc2\x85-nel.txt",
	};
	fx_mkdir(fx, "/unicode");
	ntfs_inode *dir = fx_open(fx, "/unicode");
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
		char path[512];
		snprintf(path, sizeof(path), "/unicode/%s", names[i]);
		ntfs_inode *ni = fx_create_in(fx, dir, names[i], S_IFREG);
		fx_write_stream(fx, ni, path, NULL, 0, 16 + i, FILL_TEXT);
		fx_close_in(ni, dir);
	}
	fx_close(dir);

	/* a Unicode directory name with Unicode children */
	fx_mkdir(fx, "/unicode/目录-📁");
	fx_file(fx, "/unicode/目录-📁/内容.txt", 77, FILL_TEXT, 0);

	/* 255-unit names: ASCII, and CJK (255 UTF-16 units = 765 UTF-8 bytes) */
	fx_mkdir(fx, "/long");
	{
		char name[256 * 4 + 1];
		memset(name, 'a', 251);
		memcpy(name + 251, ".txt", 5);
		char path[1200];
		snprintf(path, sizeof(path), "/long/%s", name);
		fx_file(fx, path, 300, FILL_TEXT, 0);

		size_t p = 0;
		for (int i = 0; i < 251; i++) {	/* 251 x U+4E00.. + ".txt" = 255 units */
			uint32_t cp = 0x4E00 + (i * 37) % 0x2000;
			name[p++] = 0xE0 | (cp >> 12);
			name[p++] = 0x80 | ((cp >> 6) & 63);
			name[p++] = 0x80 | (cp & 63);
		}
		memcpy(name + p, ".txt", 5);
		snprintf(path, sizeof(path), "/long/%s", name);
		fx_file(fx, path, 301, FILL_TEXT, 0);

		/* 254 + 1 directory name too */
		memset(name, 'd', 255);
		name[255] = 0;
		snprintf(path, sizeof(path), "/long/%s", name);
		fx_mkdir(fx, path);
		char inner[1400];
		snprintf(inner, sizeof(inner), "%s/inner.txt", path);
		fx_file(fx, inner, 9, FILL_TEXT, 0);
	}

	/* 5,000 entries in one directory: multi-level $INDEX_ALLOCATION B+-tree */
	fx_mkdir(fx, "/big");
	dir = fx_open(fx, "/big");
	for (int i = 0; i < 5000; i++) {
		char name[80], path[96];
		/* mix name lengths and orderings so the index is not inserted in sorted order */
		int k = (i * 7919) % 5000;
		snprintf(name, sizeof(name), "entry-%04d-%s.dat", k, (k % 3) ? "x" : "longer-suffix-to-vary-entry-size");
		snprintf(path, sizeof(path), "/big/%s", name);
		ntfs_inode *ni = fx_create_in(fx, dir, name, S_IFREG);
		if (k % 50 == 0)
			fx_write_stream(fx, ni, path, NULL, 0, 700 + k, FILL_RANDOM);	/* some non-resident */
		else if (k % 2)
			fx_write_stream(fx, ni, path, NULL, 0, k % 200, FILL_TEXT);	/* resident */
		fx_close_in(ni, dir);
	}
	fx_close(dir);
	/* and a few subdirectories inside the big one */
	for (int i = 0; i < 20; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/big/subdir-%02d", i);
		fx_mkdir(fx, path);
	}
}

static void fixture_compressed_sparse(struct fx *fx)
{
	fx_mkdir(fx, "/compressed");
	/* per-file compression flag (Windows "compress contents" on a file) */
	fx_file(fx, "/compressed/text-1m.txt", 1 * MiB, FILL_TEXT, FXF_COMPRESSED);
	fx_file(fx, "/compressed/text-100k.txt", 100 * KiB, FILL_TEXT, FXF_COMPRESSED);
	fx_file(fx, "/compressed/zeros-4m.bin", 4 * MiB, FILL_ZERO, FXF_COMPRESSED);
	fx_file(fx, "/compressed/random-1m.bin", 1 * MiB, FILL_RANDOM, FXF_COMPRESSED);	/* incompressible */
	fx_file(fx, "/compressed/random-70000.bin", 70000, FILL_RANDOM, FXF_COMPRESSED);
	fx_file(fx, "/compressed/small-resident.txt", 200, FILL_TEXT, FXF_COMPRESSED);	/* resident */
	fx_file(fx, "/compressed/one-cu-plus-1.bin", 65537, FILL_TEXT, FXF_COMPRESSED);	/* 16 clusters + 1 byte */
	fx_file(fx, "/compressed/odd-size.txt", 123457, FILL_TEXT, FXF_COMPRESSED);
	fx_file(fx, "/compressed/empty.txt", 0, FILL_TEXT, FXF_COMPRESSED);
	/* mixed: compressible and incompressible blocks alternate within one file */
	{
		ntfs_inode *ni = fx_create(fx, "/compressed/mixed-2m.bin", S_IFREG);
		fx_mark_compressed(ni);
		for (int i = 0; i < 32; i++)
			fx_write_stream(fx, ni, "/compressed/mixed-2m.bin", NULL, i * 65536, 65536,
					(i % 2) ? FILL_RANDOM : FILL_TEXT);
		fx_close(ni);
	}
	/* compressed file with a compressed ADS */
	{
		ntfs_inode *ni = fx_file_open(fx, "/compressed/with-ads.txt", 200 * KiB, FILL_TEXT, FXF_COMPRESSED);
		fx_write_stream(fx, ni, "/compressed/with-ads.txt", "stream", 0, 150 * KiB, FILL_TEXT);
		fx_close(ni);
	}
	/* a directory marked compressed: children inherit the flag (Windows semantics) */
	fx_mkdir(fx, "/compressed/inherit");
	{
		ntfs_inode *d = fx_open(fx, "/compressed/inherit");
		fx_dir_set_compressed(d);
		fx_close(d);
	}
	fx_file(fx, "/compressed/inherit/child-text.txt", 300 * KiB, FILL_TEXT, 0);
	fx_file(fx, "/compressed/inherit/child-random.bin", 300 * KiB, FILL_RANDOM, 0);
	fx_mkdir(fx, "/compressed/inherit/sub");
	fx_file(fx, "/compressed/inherit/sub/grandchild.txt", 50 * KiB, FILL_TEXT, 0);

	fx_mkdir(fx, "/sparse");
	/* data, hole, data, hole, data, trailing hole */
	{
		const char *p = "/sparse/holes-8m.bin";
		ntfs_inode *ni = fx_create(fx, p, S_IFREG);
		fx_write_stream(fx, ni, p, NULL, 0, 64 * KiB, FILL_RANDOM);
		fx_write_stream(fx, ni, p, NULL, 2 * MiB, 100 * KiB, FILL_RANDOM);
		fx_write_stream(fx, ni, p, NULL, 5 * MiB + 100, 4096 + 1, FILL_TEXT);	/* unaligned */
		fx_truncate(ni, 8 * MiB);
		fx_close(ni);
	}
	/* hole-only: size set by truncate, nothing ever written (initialized_size 0) */
	{
		ntfs_inode *ni = fx_create(fx, "/sparse/all-hole-4m.bin", S_IFREG);
		fx_truncate(ni, 4 * MiB);
		fx_close(ni);
	}
	/* leading hole, data at the end */
	{
		const char *p = "/sparse/tail-data.bin";
		ntfs_inode *ni = fx_create(fx, p, S_IFREG);
		fx_write_stream(fx, ni, p, NULL, 3 * MiB, 4096, FILL_RANDOM);
		fx_close(ni);
	}
	/* sparse and compressed at once */
	{
		const char *p = "/sparse/compressed-holes.bin";
		ntfs_inode *ni = fx_create(fx, p, S_IFREG);
		fx_mark_compressed(ni);
		fx_write_stream(fx, ni, p, NULL, 0, 64 * KiB, FILL_TEXT);
		fx_write_stream(fx, ni, p, NULL, 1 * MiB, 64 * KiB, FILL_TEXT);
		fx_truncate(ni, 3 * MiB);
		fx_close(ni);
	}
	/* big sparse: 1 GiB logical, a few KiB physical */
	{
		const char *p = "/sparse/huge-1g.bin";
		ntfs_inode *ni = fx_create(fx, p, S_IFREG);
		fx_write_stream(fx, ni, p, NULL, 0, 4096, FILL_RANDOM);
		fx_write_stream(fx, ni, p, NULL, 512 * MiB, 4096, FILL_RANDOM);
		fx_truncate(ni, 1024 * MiB);
		fx_close(ni);
	}
}

static void fixture_attrlist(struct fx *fx)
{
	fx_mkdir(fx, "/attrlist");
	/* (a) many named streams: resident $DATA records overflow the 1 KiB
	 *     base record -> libntfs-3g adds $ATTRIBUTE_LIST and extent records */
	{
		const char *p = "/attrlist/many-ads.bin";
		ntfs_inode *ni = fx_file_open(fx, p, 1000, FILL_RANDOM, 0);
		for (int i = 0; i < 40; i++) {
			char sname[32];
			snprintf(sname, sizeof(sname), "stream%02d", i);
			fx_write_stream(fx, ni, p, sname, 0, 50 + i * 7, (i % 2) ? FILL_RANDOM : FILL_TEXT);
		}
		fx_close(ni);
	}
	/* (b) heavily fragmented file. The libntfs-3g allocator (lcnalloc.c)
	 *     takes an exact LCN only as the "seek from" hint of an *append*
	 *     (end of the previous extent); every other allocation, and any
	 *     append whose hint is in use, goes to the start of the largest
	 *     free range in the bitmap. So: reserve a contiguous region with a
	 *     file, fill the rest of the volume, delete the reservation, and
	 *     append one cluster at a time alternately to two files. Each
	 *     append finds its hint taken by the other file and moves to the
	 *     start of the largest free range = the next cluster: perfect
	 *     interleaving. 600 one-cluster extents need ~1.8 KiB of mapping
	 *     pairs, more than the 1 KiB base record: libntfs-3g splits $DATA
	 *     into extent records and adds $ATTRIBUTE_LIST. */
	{
		const int nfrag = 600;
		uint32_t cs = fx->vol->cluster_size;
		const char *rp = "/attrlist/.reserve", *fp = "/attrlist/.fill";
		ntfs_inode *r = fx_create(fx, rp, S_IFREG);
		fx_write_stream(fx, r, rp, NULL, 0, (uint64_t)2 * nfrag * cs, FILL_ZERO);
		fx->bytes -= (uint64_t)2 * nfrag * cs;
		{
			ntfs_attr *na = ntfs_attr_open(r, AT_DATA, AT_UNNAMED, 0);
			if (!na || ntfs_attr_map_whole_runlist(na))
				die("map runlist of %s", rp);
			int runs = 0;
			for (runlist_element *rl = na->rl; rl->length; rl++)
				runs++;
			ntfs_attr_close(na);
			if (runs != 1)
				die("%s: expected 1 run, got %d (volume too fragmented?)", rp, runs);
		}
		fx_close(r);

		/* fill everything else, leaving a few clusters for index blocks */
		ntfs_inode *f = fx_create(fx, fp, S_IFREG);
		{
			if (ntfs_volume_get_free_space(fx->vol))
				die("ntfs_volume_get_free_space");
			s64 nfree = fx->vol->free_clusters - 32;
			if (nfree <= 0)
				die("no free space to fill");
			ntfs_attr *na = ntfs_attr_open(f, AT_DATA, AT_UNNAMED, 0);
			uint8_t *z = calloc(64, cs);
			if (!na || !z)
				die("fill open");
			for (s64 pos = 0; pos < nfree * (s64)cs;) {
				s64 n = nfree * (s64)cs - pos;
				if (n > 64 * (s64)cs)
					n = 64 * (s64)cs;
				if (ntfs_attr_pwrite(na, pos, n, z) != n)
					die("fill pwrite at %lld", (long long)pos);
				pos += n;
			}
			free(z);
			ntfs_attr_close(na);
		}
		fx_close(f);

		/* free the reserved region again */
		{
			ntfs_inode *dir = fx_open(fx, "/attrlist");
			ntfs_inode *ni = ntfs_pathname_to_inode(fx->vol, dir, ".reserve");
			ntfschar *uname = NULL;
			int ulen = ucs(".reserve", &uname);
			if (!ni || ntfs_delete(fx->vol, NULL, ni, dir, uname, (u8)ulen))	/* closes ni */
				die("ntfs_delete(%s)", rp);
			free(uname);
			fx_close(dir);
			fx->files--;
		}

		const char *pa = "/attrlist/fragmented.bin", *pb = "/attrlist/interleaver.bin";
		ntfs_inode *a = fx_create(fx, pa, S_IFREG), *b = fx_create(fx, pb, S_IFREG);
		ntfs_attr *na = ntfs_attr_open(a, AT_DATA, AT_UNNAMED, 0);
		ntfs_attr *nb = ntfs_attr_open(b, AT_DATA, AT_UNNAMED, 0);
		if (!na || !nb)
			die("attr_open fragmented");
		uint8_t *buf = malloc(cs);
		uint64_t sa = seed_for(pa, NULL), sb = seed_for(pb, NULL);
		/* start the bitmap search at the beginning of the data zone so
		 * the first allocation of each file sees the reserved region */
		fx->vol->data1_zone_pos = fx->vol->mft_zone_end;
		for (int i = 0; i < nfrag; i++) {
			fill_buf(buf, cs, &sa, FILL_RANDOM, (uint64_t)i * cs);
			if (ntfs_attr_pwrite(na, (s64)i * cs, cs, buf) != cs)
				die("pwrite %s", pa);
			fill_buf(buf, cs, &sb, FILL_RANDOM, (uint64_t)i * cs);
			if (ntfs_attr_pwrite(nb, (s64)i * cs, cs, buf) != cs)
				die("pwrite %s", pb);
		}
		free(buf);
		fx->bytes += 2ULL * nfrag * cs;
		if (ntfs_attr_map_whole_runlist(na))
			die("map runlist of %s", pa);
		int runs = 0;
		for (runlist_element *rl = na->rl; rl->length; rl++)
			runs++;
		ntfs_attr_close(na);
		ntfs_attr_close(nb);
		bool has_attrlist = NInoAttrList(a);
		fx_close(a);
		fx_close(b);
		if (runs < nfrag / 2)
			die("fragmentation failed: %s has only %d runs", pa, runs);
		if (!has_attrlist)
			die("%s has %d runs but no $ATTRIBUTE_LIST", pa, runs);

		/* give the space back (the interleaver stays: it keeps the holes) */
		{
			ntfs_inode *dir = fx_open(fx, "/attrlist");
			ntfs_inode *ni = ntfs_pathname_to_inode(fx->vol, dir, ".fill");
			ntfschar *uname = NULL;
			int ulen = ucs(".fill", &uname);
			if (!ni || ntfs_delete(fx->vol, NULL, ni, dir, uname, (u8)ulen))
				die("ntfs_delete(%s)", fp);
			free(uname);
			fx_close(dir);
			fx->files--;
		}
	}
	/* (c) both: fragmented and many streams, plus hard links (each name is
	 *     another $FILE_NAME in the base record, squeezing $DATA out) */
	{
		const char *p = "/attrlist/kitchen-sink.bin";
		ntfs_inode *ni = fx_file_open(fx, p, 300000, FILL_TEXT, 0);
		for (int i = 0; i < 12; i++) {
			char sname[40];
			snprintf(sname, sizeof(sname), "alternate-stream-number-%02d", i);
			fx_write_stream(fx, ni, p, sname, 0, 5000 + i * 100, FILL_RANDOM);
		}
		fx_close(ni);
		for (int i = 0; i < 6; i++) {
			char lp[80];
			snprintf(lp, sizeof(lp), "/attrlist/kitchen-sink-link-%d.bin", i);
			fx_link(fx, p, lp);
		}
	}
}

static void fixture_empty(struct fx *fx) { (void)fx; }

struct fixture {
	const char *name;
	uint64_t size;
	uint32_t cluster;
	const char *label;
	void (*populate)(struct fx *);
};

static const struct fixture fixtures[] = {
	{ "basic-4k",          256 * MiB, 4096,  "BASIC4K",   fixture_basic_4k },
	{ "names",             128 * MiB, 4096,  "NAMES",     fixture_names },
	{ "compressed-sparse", 128 * MiB, 4096,  "COMPSPARSE", fixture_compressed_sparse },
	{ "cluster-512",       128 * MiB, 512,   "CLUSTER512", fixture_cluster_512 },
	{ "cluster-64k",       256 * MiB, 65536, "CLUSTER64K", fixture_cluster_64k },
	{ "attrlist",          64 * MiB,  4096,  "ATTRLIST",  fixture_attrlist },
	{ "empty",             32 * MiB,  4096,  "EMPTY",     fixture_empty },
};

static int build(const struct fixture *f)
{
	struct fx fx = { .name = f->name, .size = f->size, .cluster = f->cluster, .label = f->label };
	snprintf(fx.img, sizeof(fx.img), "%s/%s.img", g_outdir, f->name);
	snprintf(fx.manifest, sizeof(fx.manifest), "%s/%s.manifest.json", g_outdir, f->name);
	if (!g_force && access(fx.img, R_OK) == 0 && access(fx.manifest, R_OK) == 0) {
		printf("%-18s exists (use --force to regenerate)\n", f->name);
		return 0;
	}
	double t0 = now_s();
	fx_begin(&fx);
	f->populate(&fx);
	fx_unmount(&fx);
	double t1 = now_s();
	fx_write_manifest(&fx);
	double t2 = now_s();
	struct stat st;
	if (stat(fx.img, &st))
		die("stat %s", fx.img);
	printf("%-18s %4" PRIu64 " MiB image (%5.1f MiB on disk), cluster %6u, %5" PRIu64 " files, %4" PRIu64
	       " dirs, %7.1f MiB data: populate %.2fs, manifest %.2fs\n",
	       f->name, f->size / MiB, (double)st.st_blocks * 512 / (double)MiB, f->cluster, fx.files, fx.dirs,
	       (double)fx.bytes / (double)MiB, t1 - t0, t2 - t1);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: mkfixtures [-o OUTDIR] [--mkntfs PATH] [--seed N] [--force] [-v] [NAME...]\n"
		"fixtures:");
	for (size_t i = 0; i < sizeof(fixtures) / sizeof(*fixtures); i++)
		fprintf(stderr, " %s", fixtures[i].name);
	fputc('\n', stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");
	ntfs_log_set_handler(ntfs_log_handler_stderr);
	int i;
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			g_outdir = argv[++i];
		else if (!strcmp(argv[i], "--mkntfs") && i + 1 < argc)
			g_mkntfs = argv[++i];
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
			g_seed = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f"))
			g_force = true;
		else if (!strcmp(argv[i], "-v"))
			g_verbose = true, ntfs_log_set_levels(NTFS_LOG_LEVEL_WARNING | NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR);
		else
			usage();
	}
	if (!g_verbose)
		ntfs_log_set_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR);
	mkdir(g_outdir, 0755);
	int rc = 0;
	if (i == argc) {
		for (size_t k = 0; k < sizeof(fixtures) / sizeof(*fixtures); k++)
			rc |= build(&fixtures[k]);
	} else {
		for (; i < argc; i++) {
			bool found = false;
			for (size_t k = 0; k < sizeof(fixtures) / sizeof(*fixtures); k++)
				if (!strcmp(fixtures[k].name, argv[i])) {
					rc |= build(&fixtures[k]);
					found = true;
				}
			if (!found) {
				fprintf(stderr, "unknown fixture %s\n", argv[i]);
				rc = 2;
			}
		}
	}
	return rc;
}
