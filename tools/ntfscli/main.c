/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ntfscli - exercise the NTFS core (core/include/ntfscore.h) on an image
 * file or device without FSKit.
 *
 *   ntfscli [global options] COMMAND IMAGE [ARGS...]
 *
 * Commands
 *   probe                       ntfs_probe(): is it NTFS? print label/serial/sizes
 *   info                        mount + ntfs_volume_get_info()
 *   ls [-R] [-l] [-a] [PATH]    list a directory (default /); -l long, -a system files
 *   stat PATH                   every ntfs_attr field + xattr (ADS) names
 *   cat PATH                    unnamed stream to stdout
 *   cp-out SRC DST              copy a file out of the volume (DST '-' = stdout)
 *   cp-in HOST_SRC PATH         copy a host file in (creates or replaces)
 *   mkdir PATH | rmdir PATH | rm PATH
 *   mv OLD NEW                  rename
 *   ln EXISTING NEW             hard link
 *   ln -s TARGET NEW            symlink
 *   truncate PATH SIZE
 *   xattr list PATH | get PATH NAME | set PATH NAME VALUE|@FILE | rm PATH NAME
 *   sync                        ntfs_volume_sync()
 *   verify MANIFEST [--no-data] [--no-times]
 *                               compare the whole volume with a mkfixtures manifest
 *   bench [--size MiB] [--files N] [--dir PATH]
 *                               sequential write/read + create/stat/unlink
 *
 * Global options: --ro --case-sensitive --allow-illegal --show-system
 *                 --hide-hidden --no-fallback -v (repeat for debug) -q
 *
 * Exit status: 0 ok, 1 operation failed (errno name printed), 2 usage,
 * 3 verify found mismatches.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ntfscore.h>
#include <ntfsport/bdev.h>
#include "json.h"
#include "sha256.h"

#ifndef NTFSCLI_CORE
#define NTFSCLI_CORE "unknown"
#endif
/*
 * No XATTR_CREATE / XATTR_REPLACE here, and <sys/xattr.h> is deliberately not
 * included. This file used to define them to Darwin's 2 and 4, which are the
 * wrong numbers for ntfs_setxattr(): the core reads 2 as REPLACE and 4 as no
 * flags at all. Nothing used them, so nothing broke, but the next person to add
 * a flag to `xattr set` would have reached for the name that was already here.
 * The flags to pass are NTFS_XATTR_CREATE / NTFS_XATTR_REPLACE from ntfscore.h.
 */

#define CHUNK (1u << 20)

#ifndef EUCLEAN
#define EUCLEAN 117
#endif
/* ------------------------------------------------------------------------ */
/* errno names                                                              */

static const struct { int e; const char *n; } errnames[] = {
	{ EPERM, "EPERM" }, { ENOENT, "ENOENT" }, { EIO, "EIO" }, { ENXIO, "ENXIO" },
	{ E2BIG, "E2BIG" }, { EBADF, "EBADF" }, { EAGAIN, "EAGAIN" }, { ENOMEM, "ENOMEM" },
	{ EACCES, "EACCES" }, { EBUSY, "EBUSY" }, { EEXIST, "EEXIST" }, { EXDEV, "EXDEV" },
	{ ENODEV, "ENODEV" }, { ENOTDIR, "ENOTDIR" }, { EISDIR, "EISDIR" }, { EINVAL, "EINVAL" },
	{ EFBIG, "EFBIG" }, { ENOSPC, "ENOSPC" }, { EROFS, "EROFS" }, { EMLINK, "EMLINK" },
	{ ERANGE, "ERANGE" }, { ENAMETOOLONG, "ENAMETOOLONG" }, { ENOSYS, "ENOSYS" },
	{ ENOTEMPTY, "ENOTEMPTY" }, { ELOOP, "ELOOP" }, { EOVERFLOW, "EOVERFLOW" },
	{ EOPNOTSUPP, "EOPNOTSUPP" }, { ENOTSUP, "ENOTSUP" }, { EILSEQ, "EILSEQ" },
#ifdef ENOATTR
	{ ENOATTR, "ENOATTR" },
#endif
#ifdef ENODATA
	{ ENODATA, "ENODATA" },
#endif
	{ EUCLEAN + 0, "EUCLEAN" },
};

static const char *errname(int e)
{
	static char buf[32];
	if (e < 0)
		e = -e;
	for (size_t i = 0; i < sizeof(errnames) / sizeof(*errnames); i++)
		if (errnames[i].e == e)
			return errnames[i].n;
	snprintf(buf, sizeof(buf), "errno%d", e);
	return buf;
}

/* ------------------------------------------------------------------------ */
/* globals / diagnostics                                                    */

static struct {
	uint32_t flags;
	int verbose;		/* -1 quiet, 0 warn, 1 info, 2 debug */
	const char *image;
	const char *cmd;
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root;
} g = { .flags = NTFS_MOUNT_RDONLY_FALLBACK };

static void logger(int level, const char *msg, void *ctx)
{
	static const char *lv[] = { "error", "warn", "info", "debug" };
	int max = g.verbose + 1;	/* -1 -> errors only, 0 -> +warn, 1 -> +info, 2 -> +debug */
	if (level > max)
		return;
	fprintf(stderr, "core[%s]: %s\n", level >= 0 && level < 4 ? lv[level] : "?", msg);
}

static int fail(int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "ntfscli: %s: ", g.cmd ? g.cmd : "");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (err)
		fprintf(stderr, ": %s (%s)", errname(err), strerror(err < 0 ? -err : err));
	fputc('\n', stderr);
	return 1;
}

static int usage(const char *why)
{
	if (why)
		fprintf(stderr, "ntfscli: %s\n", why);
	fprintf(stderr,
		"usage: ntfscli [--ro] [--case-sensitive] [--allow-illegal] [--show-system]\n"
		"               [--hide-hidden] [--no-fallback] [-v|-q] COMMAND IMAGE [ARGS]\n"
		"commands: probe | info | ls [-R] [-l] [-a] [PATH] | stat PATH | cat PATH |\n"
		"          cp-out SRC DST | cp-in HOST_SRC PATH | mkdir PATH | rm PATH | rmdir PATH |\n"
		"          mv OLD NEW | ln [-s] EXISTING|TARGET NEW | truncate PATH SIZE |\n"
		"          xattr list|get|set|rm PATH [NAME] [VALUE|@FILE] | sync |\n"
		"          verify MANIFEST [--no-data] [--no-times] |\n"
		"          bench [--size MiB] [--files N] [--dir PATH]\n"
		"core: %s\n", NTFSCLI_CORE);
	return 2;
}

/* ------------------------------------------------------------------------ */
/* mount / path helpers                                                     */

static int open_dev(bool ro)
{
	g.dev = ntfs_bdev_open_path(g.image, ro);
	if (!g.dev)
		return fail(errno, "open %s", g.image);
	return 0;
}

static int do_mount(bool ro)
{
	int rc = open_dev(ro);
	if (rc)
		return rc;
	struct ntfs_mount_options opts = {
		.flags = g.flags | (ro ? NTFS_MOUNT_RDONLY : 0),
		.uid = getuid(), .gid = getgid(), .fmask = 0022, .dmask = 0022,
	};
	int err = ntfs_mount(g.dev, &opts, &g.vol);
	if (err)
		return fail(err, "mount %s", g.image);
	err = ntfs_volume_root(g.vol, &g.root);
	if (err)
		return fail(err, "root inode");
	return 0;
}

static int do_unmount(void)
{
	int rc = 0;
	if (g.root)
		ntfs_inode_put(g.root);
	g.root = NULL;
	if (g.vol) {
		int err = ntfs_unmount(g.vol);
		if (err)
			rc = fail(err, "unmount");
	}
	g.vol = NULL;
	ntfs_bdev_close(g.dev);
	g.dev = NULL;
	return rc;
}

/* Resolve @path (absolute, '/'-separated) to a referenced inode. */
static int resolve(const char *path, ntfs_inode_t **out)
{
	ntfs_inode_t *cur = g.root;
	ntfs_inode_ref(cur);
	char *dup = strdup(path), *save = NULL;
	for (char *tok = strtok_r(dup, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (!strcmp(tok, "."))
			continue;
		ntfs_inode_t *next = NULL;
		int err = ntfs_lookup(cur, tok, &next);
		ntfs_inode_put(cur);
		if (err) {
			free(dup);
			return err;
		}
		cur = next;
	}
	free(dup);
	*out = cur;
	return 0;
}

/* Resolve the parent of @path and return the final component. */
static int resolve_parent(const char *path, ntfs_inode_t **dir, const char **base)
{
	const char *slash = strrchr(path, '/');
	if (!slash)
		return -EINVAL;
	*base = slash + 1;
	if (!**base)
		return -EINVAL;
	char *parent = strndup(path, slash == path ? 1 : (size_t)(slash - path));
	int err = resolve(parent, dir);
	free(parent);
	return err;
}

static const char *type_name(int t)
{
	switch (t) {
	case NTFS_ITEM_FILE: return "file";
	case NTFS_ITEM_DIR: return "dir";
	case NTFS_ITEM_SYMLINK: return "symlink";
	case NTFS_ITEM_OTHER: return "other";
	default: return "?";
	}
}

static void fmt_time(struct ntfs_timespec t, char out[40])
{
	struct tm tm;
	time_t sec = (time_t)t.sec;
	gmtime_r(&sec, &tm);
	snprintf(out, 40, "%04d-%02d-%02dT%02d:%02d:%02d.%07ldZ", tm.tm_year + 1900, tm.tm_mon + 1,
		 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (long)t.nsec / 100);
}

/* ------------------------------------------------------------------------ */
/* readdir into an array                                                    */

struct dent {
	char *name;
	uint64_t ino;
	int type;
	bool has_attr;
	struct ntfs_attr attr;
};
struct dlist { struct dent *v; size_t n, cap; };

static int collect_cb(const struct ntfs_dirent *e, void *ctx)
{
	struct dlist *l = ctx;
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		l->v = realloc(l->v, l->cap * sizeof(*l->v));
		if (!l->v)
			return -ENOMEM;
	}
	struct dent *d = &l->v[l->n++];
	d->name = strndup(e->name, e->name_len);
	d->ino = e->inode_no;
	d->type = e->type;
	d->has_attr = e->has_attr;
	if (e->has_attr)
		d->attr = e->attr;
	return 0;
}

static int dent_cmp(const void *a, const void *b)
{
	return strcmp(((const struct dent *)a)->name, ((const struct dent *)b)->name);
}

static int read_dir(ntfs_inode_t *dir, bool want_attr, struct dlist *l)
{
	uint64_t cookie = 0;
	bool eof = false;
	memset(l, 0, sizeof(*l));
	while (!eof) {
		int err = ntfs_readdir(dir, &cookie, want_attr, collect_cb, l, &eof);
		if (err)
			return err;
	}
	qsort(l->v, l->n, sizeof(*l->v), dent_cmp);
	return 0;
}

static void free_dir(struct dlist *l)
{
	for (size_t i = 0; i < l->n; i++)
		free(l->v[i].name);
	free(l->v);
}

static bool is_dotdir(const char *n) { return !strcmp(n, ".") || !strcmp(n, ".."); }

/* ------------------------------------------------------------------------ */
/* data helpers                                                             */

static int read_all(ntfs_inode_t *ni, uint64_t size, int (*sink)(const void *, size_t, void *), void *ctx)
{
	static uint8_t *buf;
	if (!buf && !(buf = malloc(CHUNK)))
		return -ENOMEM;
	uint64_t pos = 0;
	while (pos < size) {
		size_t n = size - pos < CHUNK ? (size_t)(size - pos) : CHUNK;
		ssize_t r = ntfs_read(ni, buf, n, pos);
		if (r < 0)
			return (int)r;
		if (r == 0)
			return -EIO;	/* short read before the size we were told */
		int rc = sink(buf, (size_t)r, ctx);
		if (rc)
			return rc;
		pos += (uint64_t)r;
	}
	return 0;
}

static int sink_sha(const void *b, size_t n, void *ctx) { sha256_update(ctx, b, n); return 0; }
static int sink_fd(const void *b, size_t n, void *ctx)
{
	int fd = *(int *)ctx;
	const char *p = b;
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0)
			return -errno;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int hash_inode(ntfs_inode_t *ni, uint64_t size, char hex[65])
{
	struct sha256_ctx c;
	uint8_t d[32];
	sha256_init(&c);
	int rc = read_all(ni, size, sink_sha, &c);
	if (rc)
		return rc;
	sha256_final(&c, d);
	sha256_hex(d, hex);
	return 0;
}

static int write_all(ntfs_inode_t *ni, int fd)
{
	static uint8_t *buf;
	if (!buf && !(buf = malloc(CHUNK)))
		return -ENOMEM;
	uint64_t pos = 0;
	for (;;) {
		ssize_t r = read(fd, buf, CHUNK);
		if (r < 0)
			return -errno;
		if (r == 0)
			return 0;
		size_t done = 0;
		while (done < (size_t)r) {
			ssize_t w = ntfs_write(ni, buf + done, (size_t)r - done, pos + done);
			if (w < 0)
				return (int)w;
			if (w == 0)
				return -ENOSPC;
			done += (size_t)w;
		}
		pos += (uint64_t)r;
	}
}

/* ------------------------------------------------------------------------ */
/* commands                                                                 */

static void print_volinfo(const struct ntfs_volume_info *vi)
{
	printf("label:            %s\n", vi->label);
	printf("serial:           %016" PRIx64 "\n", vi->serial);
	printf("version:          %u.%u\n", vi->major_ver, vi->minor_ver);
	printf("cluster_size:     %u\n", vi->cluster_size);
	printf("sector_size:      %u\n", vi->sector_size);
	printf("mft_record_size:  %u\n", vi->mft_record_size);
	printf("total_clusters:   %" PRIu64 "\n", vi->total_clusters);
	printf("free_clusters:    %" PRIu64 "\n", vi->free_clusters);
	printf("total_mft_records:%" PRIu64 "\n", vi->total_mft_records);
	printf("free_mft_records: %" PRIu64 "\n", vi->free_mft_records);
	printf("read_only:        %s (reason %d)\n", vi->read_only ? "yes" : "no", vi->ro_reason);
	printf("dirty:            %s\n", vi->dirty ? "yes" : "no");
	printf("hibernated:       %s\n", vi->hibernated ? "yes" : "no");
	printf("logfile_clean:    %s\n", vi->logfile_clean ? "yes" : "no");
}

static int cmd_probe(int argc, char **argv)
{
	int rc = open_dev(true);
	if (rc)
		return rc;
	struct ntfs_volume_info vi;
	int err = ntfs_probe(g.dev, &vi);
	if (err) {
		ntfs_bdev_close(g.dev);
		return fail(err, "probe %s", g.image);
	}
	printf("%s: NTFS\n", g.image);
	print_volinfo(&vi);
	ntfs_bdev_close(g.dev);
	return 0;
}

static int cmd_info(int argc, char **argv)
{
	int rc = do_mount(true);
	if (rc)
		return rc;
	struct ntfs_volume_info vi;
	int err = ntfs_volume_get_info(g.vol, &vi);
	if (err)
		rc = fail(err, "get_info");
	else {
		printf("core:             %s (%s)\n", ntfs_core_version(), NTFSCLI_CORE);
		print_volinfo(&vi);
	}
	do_unmount();
	return rc;
}

static void print_long(const struct ntfs_attr *a, const char *name, int type)
{
	char mt[40];
	fmt_time(a->mtime, mt);
	printf("%c%c%c%c%c %2u %12" PRIu64 " %s %s%s\n",
	       type == NTFS_ITEM_DIR ? 'd' : type == NTFS_ITEM_SYMLINK ? 'l' : '-',
	       a->compressed ? 'c' : '-', a->sparse ? 's' : '-', a->has_ads ? 'a' : '-',
	       a->encrypted ? 'e' : '-', a->nlink, a->size, mt, name,
	       type == NTFS_ITEM_DIR ? "/" : "");
}

static int ls_dir(ntfs_inode_t *dir, const char *path, bool recursive, bool lng, bool all)
{
	struct dlist l;
	int err = read_dir(dir, lng, &l);
	if (err)
		return fail(err, "readdir %s", path);
	printf("%s:\n", path);
	for (size_t i = 0; i < l.n; i++) {
		struct dent *d = &l.v[i];
		if (is_dotdir(d->name))
			continue;
		if (!all && d->ino < 16 && !strcmp(path, "/"))
			continue;
		if (lng) {
			if (!d->has_attr) {	/* core did not fill it: stat explicitly */
				ntfs_inode_t *ni;
				if (!ntfs_lookup(dir, d->name, &ni)) {
					ntfs_getattr(ni, &d->attr);
					ntfs_inode_put(ni);
				}
			}
			print_long(&d->attr, d->name, d->type);
		} else {
			printf("%s%s\n", d->name, d->type == NTFS_ITEM_DIR ? "/" : "");
		}
	}
	int rc = 0;
	if (recursive) {
		for (size_t i = 0; i < l.n && !rc; i++) {
			struct dent *d = &l.v[i];
			if (d->type != NTFS_ITEM_DIR || is_dotdir(d->name))
				continue;
			if (!all && d->ino < 16)
				continue;
			ntfs_inode_t *sub;
			err = ntfs_lookup(dir, d->name, &sub);
			if (err) {
				rc = fail(err, "lookup %s/%s", path, d->name);
				break;
			}
			char child[4096];
			snprintf(child, sizeof(child), "%s/%s", strcmp(path, "/") ? path : "", d->name);
			printf("\n");
			rc = ls_dir(sub, child, true, lng, all);
			ntfs_inode_put(sub);
		}
	}
	free_dir(&l);
	return rc;
}

static int cmd_ls(int argc, char **argv)
{
	bool recursive = false, lng = false, all = false;
	const char *path = "/";
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-R")) recursive = true;
		else if (!strcmp(argv[i], "-l")) lng = true;
		else if (!strcmp(argv[i], "-a")) all = true;
		else if (!strcmp(argv[i], "-Rl") || !strcmp(argv[i], "-lR")) recursive = lng = true;
		else path = argv[i];
	}
	int rc = do_mount(true);
	if (rc)
		return rc;
	ntfs_inode_t *ni;
	int err = resolve(path, &ni);
	if (err)
		rc = fail(err, "lookup %s", path);
	else {
		struct ntfs_attr a;
		if (!ntfs_getattr(ni, &a) && a.type != NTFS_ITEM_DIR) {
			if (lng) print_long(&a, path, a.type); else printf("%s\n", path);
		} else {
			rc = ls_dir(ni, path, recursive, lng, all);
		}
		ntfs_inode_put(ni);
	}
	do_unmount();
	return rc;
}

static int cmd_stat(int argc, char **argv)
{
	if (argc < 1)
		return usage("stat needs PATH");
	int rc = do_mount(true);
	if (rc)
		return rc;
	ntfs_inode_t *ni;
	int err = resolve(argv[0], &ni);
	if (err) {
		rc = fail(err, "lookup %s", argv[0]);
		goto out;
	}
	struct ntfs_attr a;
	err = ntfs_getattr(ni, &a);
	if (err) {
		rc = fail(err, "getattr %s", argv[0]);
	} else {
		char t[4][40];
		fmt_time(a.atime, t[0]); fmt_time(a.mtime, t[1]); fmt_time(a.ctime, t[2]); fmt_time(a.crtime, t[3]);
		printf("path:        %s\ninode:       %" PRIu64 " (gen %u)\ntype:        %s\nmode:        %06o\n"
		       "nlink:       %u\nuid/gid:     %u/%u\nsize:        %" PRIu64 "\nalloc_size:  %" PRIu64 "\n"
		       "atime:       %s\nmtime:       %s\nctime:       %s\ncrtime:      %s\n"
		       "attributes:  0x%08x\nreparse_tag: 0x%08x\ncompressed:  %d\nsparse:      %d\n"
		       "encrypted:   %d\nhas_ads:     %d\n",
		       argv[0], a.inode_no, a.generation, type_name(a.type), a.mode, a.nlink, a.uid, a.gid,
		       a.size, a.alloc_size, t[0], t[1], t[2], t[3], a.file_attributes, a.reparse_tag,
		       a.compressed, a.sparse, a.encrypted, a.has_ads);
		size_t len = 0;
		if (!ntfs_listxattr(ni, NULL, 0, &len) && len) {
			char *buf = malloc(len);
			if (!ntfs_listxattr(ni, buf, len, &len)) {
				printf("streams:    ");
				for (size_t off = 0; off < len; off += strlen(buf + off) + 1)
					printf(" %s", buf + off);
				printf("\n");
			}
			free(buf);
		}
		if (a.type == NTFS_ITEM_SYMLINK) {
			char tgt[4096];
			size_t tl;
			if (!ntfs_readlink(ni, tgt, sizeof(tgt), &tl))
				printf("target:      %.*s\n", (int)tl, tgt);
		}
	}
	ntfs_inode_put(ni);
out:
	do_unmount();
	return rc;
}

static int cat_to_fd(const char *path, int fd)
{
	ntfs_inode_t *ni;
	int err = resolve(path, &ni);
	if (err)
		return fail(err, "lookup %s", path);
	struct ntfs_attr a;
	err = ntfs_getattr(ni, &a);
	if (!err && a.type == NTFS_ITEM_DIR)
		err = -EISDIR;
	if (!err)
		err = read_all(ni, a.size, sink_fd, &fd);
	ntfs_inode_put(ni);
	return err ? fail(err, "read %s", path) : 0;
}

static int cmd_cat(int argc, char **argv)
{
	if (argc < 1)
		return usage("cat needs PATH");
	int rc = do_mount(true);
	if (rc)
		return rc;
	rc = cat_to_fd(argv[0], 1);
	do_unmount();
	return rc;
}

static int cmd_cp_out(int argc, char **argv)
{
	if (argc < 2)
		return usage("cp-out needs SRC DST");
	int rc = do_mount(true);
	if (rc)
		return rc;
	int fd = 1;
	if (strcmp(argv[1], "-")) {
		fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			rc = fail(errno, "open %s", argv[1]);
			goto out;
		}
	}
	rc = cat_to_fd(argv[0], fd);
	if (fd != 1)
		close(fd);
out:
	do_unmount();
	return rc;
}

static int cmd_cp_in(int argc, char **argv)
{
	if (argc < 2)
		return usage("cp-in needs HOST_SRC PATH");
	int fd = open(argv[0], O_RDONLY);
	if (fd < 0)
		return fail(errno, "open %s", argv[0]);
	int rc = do_mount(false);
	if (rc) {
		close(fd);
		return rc;
	}
	ntfs_inode_t *dir, *ni = NULL;
	const char *base;
	int err = resolve_parent(argv[1], &dir, &base);
	if (err) {
		rc = fail(err, "parent of %s", argv[1]);
		goto out;
	}
	err = ntfs_lookup(dir, base, &ni);
	if (err == -ENOENT)
		err = ntfs_create(dir, base, 0644, &ni);
	else if (!err)
		err = ntfs_truncate(ni, 0);
	if (err) {
		rc = fail(err, "create %s", argv[1]);
	} else {
		err = write_all(ni, fd);
		if (!err)
			err = ntfs_fsync(ni, false);
		if (err)
			rc = fail(err, "write %s", argv[1]);
	}
	if (ni)
		ntfs_inode_put(ni);
	ntfs_inode_put(dir);
out:
	close(fd);
	int urc = do_unmount();
	return rc ? rc : urc;
}

/* mkdir / rm / rmdir / mv / ln / truncate share a "mount rw, act, unmount" shape */
static int cmd_simple(int argc, char **argv)
{
	const char *c = g.cmd;
	int need = !strcmp(c, "mv") || !strcmp(c, "ln") || !strcmp(c, "truncate") ? 2 : 1;
	bool symlink = false;
	if (!strcmp(c, "ln") && argc && !strcmp(argv[0], "-s")) {
		symlink = true;
		argv++;
		argc--;
	}
	if (argc < need)
		return usage("missing argument");
	int rc = do_mount(false);
	if (rc)
		return rc;
	ntfs_inode_t *dir = NULL, *dir2 = NULL, *ni = NULL;
	const char *base = NULL, *base2 = NULL;
	int err;

	if (!strcmp(c, "mkdir")) {
		if (!(err = resolve_parent(argv[0], &dir, &base))) {
			err = ntfs_mkdir(dir, base, 0755, &ni);
			if (ni) ntfs_inode_put(ni);
		}
	} else if (!strcmp(c, "rm")) {
		if (!(err = resolve_parent(argv[0], &dir, &base)))
			err = ntfs_unlink(dir, base);
	} else if (!strcmp(c, "rmdir")) {
		if (!(err = resolve_parent(argv[0], &dir, &base)))
			err = ntfs_rmdir(dir, base);
	} else if (!strcmp(c, "mv")) {
		if (!(err = resolve_parent(argv[0], &dir, &base)) &&
		    !(err = resolve_parent(argv[1], &dir2, &base2)))
			err = ntfs_rename(dir, base, dir2, base2);
	} else if (!strcmp(c, "ln")) {
		if (symlink) {
			if (!(err = resolve_parent(argv[1], &dir, &base))) {
				err = ntfs_symlink(dir, base, argv[0], &ni);
				if (ni) ntfs_inode_put(ni);
			}
		} else if (!(err = resolve(argv[0], &ni)) &&
			   !(err = resolve_parent(argv[1], &dir, &base))) {
			err = ntfs_link(ni, dir, base);
		}
		if (ni && !symlink) ntfs_inode_put(ni);
	} else if (!strcmp(c, "truncate")) {
		if (!(err = resolve(argv[0], &ni))) {
			err = ntfs_truncate(ni, strtoull(argv[1], NULL, 0));
			ntfs_inode_put(ni);
		}
	} else {
		err = -EINVAL;
	}
	if (dir) ntfs_inode_put(dir);
	if (dir2) ntfs_inode_put(dir2);
	if (err)
		rc = fail(err, "%s %s", c, argv[0]);
	int urc = do_unmount();
	return rc ? rc : urc;
}

static int cmd_xattr(int argc, char **argv)
{
	if (argc < 2)
		return usage("xattr needs list|get|set|rm PATH [NAME] [VALUE|@FILE]");
	const char *op = argv[0], *path = argv[1], *name = argc > 2 ? argv[2] : NULL;
	bool rw = !strcmp(op, "set") || !strcmp(op, "rm");
	if ((rw || !strcmp(op, "get")) && !name)
		return usage("xattr: NAME required");
	int rc = do_mount(!rw);
	if (rc)
		return rc;
	ntfs_inode_t *ni;
	int err = resolve(path, &ni);
	if (err) {
		rc = fail(err, "lookup %s", path);
		goto out;
	}
	if (!strcmp(op, "list")) {
		size_t len = 0;
		err = ntfs_listxattr(ni, NULL, 0, &len);
		char *buf = len ? malloc(len) : NULL;
		if (!err && len)
			err = ntfs_listxattr(ni, buf, len, &len);
		if (!err)
			for (size_t off = 0; off < len; off += strlen(buf + off) + 1)
				printf("%s\n", buf + off);
		free(buf);
	} else if (!strcmp(op, "get")) {
		size_t len = 0;
		err = ntfs_getxattr(ni, name, NULL, 0, &len);
		void *buf = len ? malloc(len) : NULL;
		if (!err && len)
			err = ntfs_getxattr(ni, name, buf, len, &len);
		if (!err && len)
			err = sink_fd(buf, len, &(int){1});
		free(buf);
	} else if (!strcmp(op, "set")) {
		if (argc < 4) {
			rc = usage("xattr set needs VALUE or @FILE");
			ntfs_inode_put(ni);
			goto out;
		}
		void *val = argv[3];
		size_t len = strlen(argv[3]);
		if (argv[3][0] == '@') {
			val = json_read_file(argv[3] + 1, &len);
			if (!val) {
				rc = fail(errno, "read %s", argv[3] + 1);
				ntfs_inode_put(ni);
				goto out;
			}
		}
		err = ntfs_setxattr(ni, name, val, len, 0);
		if (val != argv[3])
			free(val);
	} else if (!strcmp(op, "rm")) {
		err = ntfs_removexattr(ni, name);
	} else {
		err = -EINVAL;
	}
	ntfs_inode_put(ni);
	if (err)
		rc = fail(err, "xattr %s %s %s", op, path, name ? name : "");
out:
	{
		int urc = do_unmount();
		return rc ? rc : urc;
	}
}

static int cmd_sync(int argc, char **argv)
{
	int rc = do_mount(false);
	if (rc)
		return rc;
	int err = ntfs_volume_sync(g.vol);
	if (err)
		rc = fail(err, "sync");
	int urc = do_unmount();
	return rc ? rc : urc;
}

/* ---- verify -------------------------------------------------------------- */

struct vstats { int checked, mismatched, missing, extra; bool data, times; };

static void mismatch(struct vstats *s, const char *path, const char *fmt, ...)
{
	va_list ap;
	s->mismatched++;
	fprintf(stderr, "MISMATCH %s: ", path);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static bool parse_iso(const char *s, struct ntfs_timespec *out)
{
	int y, mo, d, h, mi, se;
	long frac = 0;
	int n = sscanf(s, "%d-%d-%dT%d:%d:%d.%7ldZ", &y, &mo, &d, &h, &mi, &se, &frac);
	if (n < 6)
		return false;
	struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = se };
	out->sec = (int64_t)timegm(&tm);
	out->nsec = (int32_t)(frac * 100);
	return true;
}

/* set of manifest paths, for the "extra entries" pass */
struct pathset { char **v; size_t n; };
static int pcmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static bool pathset_has(struct pathset *ps, const char *p)
{
	return bsearch(&p, ps->v, ps->n, sizeof(char *), pcmp) != NULL;
}

static void verify_entry(struct vstats *st, const struct json_value *e)
{
	const char *path = json_get_str(e, "path", NULL);
	if (!path)
		return;
	st->checked++;
	ntfs_inode_t *ni;
	int err = resolve(path, &ni);
	if (err) {
		st->missing++;
		mismatch(st, path, "lookup failed: %s", errname(err));
		return;
	}
	struct ntfs_attr a;
	err = ntfs_getattr(ni, &a);
	if (err) {
		mismatch(st, path, "getattr: %s", errname(err));
		ntfs_inode_put(ni);
		return;
	}
	const char *want_type = json_get_str(e, "type", "file");
	if (strcmp(type_name(a.type), want_type))
		mismatch(st, path, "type %s, manifest %s", type_name(a.type), want_type);
	if (a.type != NTFS_ITEM_DIR) {
		uint64_t want_size = (uint64_t)json_get_num(e, "size", 0);
		if (a.size != want_size)
			mismatch(st, path, "size %" PRIu64 ", manifest %" PRIu64, a.size, want_size);
	}
	uint32_t want_nlink = (uint32_t)json_get_num(e, "nlink", 1);
	if (a.nlink != want_nlink)
		mismatch(st, path, "nlink %u, manifest %u", a.nlink, want_nlink);
	if (a.compressed != json_get_bool(e, "compressed", false))
		mismatch(st, path, "compressed %d, manifest %d", a.compressed, json_get_bool(e, "compressed", false));
	if (a.sparse != json_get_bool(e, "sparse", false))
		mismatch(st, path, "sparse %d, manifest %d", a.sparse, json_get_bool(e, "sparse", false));
	if (json_get(e, "mft_no") && a.inode_no != (uint64_t)json_get_num(e, "mft_no", 0))
		mismatch(st, path, "inode %" PRIu64 ", manifest %.0f", a.inode_no, json_get_num(e, "mft_no", 0));
	if (st->times) {
		struct ntfs_timespec want;
		const char *ms = json_get_str(e, "mtime", NULL);
		if (ms && parse_iso(ms, &want) && (want.sec != a.mtime.sec || want.nsec != a.mtime.nsec)) {
			char got[40];
			fmt_time(a.mtime, got);
			mismatch(st, path, "mtime %s, manifest %s", got, ms);
		}
		ms = json_get_str(e, "crtime", NULL);
		if (ms && parse_iso(ms, &want) && (want.sec != a.crtime.sec || want.nsec != a.crtime.nsec)) {
			char got[40];
			fmt_time(a.crtime, got);
			mismatch(st, path, "crtime %s, manifest %s", got, ms);
		}
	}
	if (st->data && a.type != NTFS_ITEM_DIR) {
		const char *want = json_get_str(e, "sha256", NULL);
		char hex[65];
		err = hash_inode(ni, a.size, hex);
		if (err)
			mismatch(st, path, "read: %s", errname(err));
		else if (want && strcmp(hex, want))
			mismatch(st, path, "sha256 %s, manifest %s", hex, want);
	}
	/* streams */
	struct json_value *streams = json_get(e, "streams");
	size_t want_n = streams && streams->type == JSON_ARRAY ? streams->u.arr.n : 0;
	size_t len = 0;
	err = ntfs_listxattr(ni, NULL, 0, &len);
	char *names = NULL;
	size_t got_n = 0;
	if (!err && len) {
		names = malloc(len);
		err = ntfs_listxattr(ni, names, len, &len);
	}
	if (err) {
		mismatch(st, path, "listxattr: %s", errname(err));
	} else {
		for (size_t off = 0; off < len; off += strlen(names + off) + 1)
			got_n++;
		if (got_n != want_n)
			mismatch(st, path, "%zu streams, manifest %zu", got_n, want_n);
		if (a.has_ads != (want_n > 0))
			mismatch(st, path, "has_ads %d, manifest %s", a.has_ads, want_n ? "true" : "false");
		for (size_t i = 0; i < want_n; i++) {
			const struct json_value *s = streams->u.arr.items[i];
			const char *sname = json_get_str(s, "name", "");
			uint64_t ssize = (uint64_t)json_get_num(s, "size", 0);
			size_t slen = 0;
			int e2 = ntfs_getxattr(ni, sname, NULL, 0, &slen);
			if (e2) {
				mismatch(st, path, "stream %s: %s", sname, errname(e2));
				continue;
			}
			if (slen != ssize) {
				mismatch(st, path, "stream %s size %zu, manifest %" PRIu64, sname, slen, ssize);
				continue;
			}
			if (st->data && slen) {
				void *buf = malloc(slen);
				e2 = ntfs_getxattr(ni, sname, buf, slen, &slen);
				if (e2) {
					mismatch(st, path, "stream %s read: %s", sname, errname(e2));
				} else {
					char hex[65];
					sha256_buf_hex(buf, slen, hex);
					const char *want = json_get_str(s, "sha256", "");
					if (strcmp(hex, want))
						mismatch(st, path, "stream %s sha256 %s, manifest %s", sname, hex, want);
				}
				free(buf);
			}
		}
	}
	free(names);
	ntfs_inode_put(ni);
}

static void verify_extra(struct vstats *st, struct pathset *ps, ntfs_inode_t *dir, const char *path)
{
	struct dlist l;
	if (read_dir(dir, false, &l))
		return;
	for (size_t i = 0; i < l.n; i++) {
		struct dent *d = &l.v[i];
		if (is_dotdir(d->name) || d->ino < 16)
			continue;
		char child[4096];
		snprintf(child, sizeof(child), "%s/%s", strcmp(path, "/") ? path : "", d->name);
		if (!pathset_has(ps, child)) {
			st->extra++;
			mismatch(st, child, "present on volume but not in manifest");
		}
		if (d->type == NTFS_ITEM_DIR) {
			ntfs_inode_t *sub;
			if (!ntfs_lookup(dir, d->name, &sub)) {
				verify_extra(st, ps, sub, child);
				ntfs_inode_put(sub);
			}
		}
	}
	free_dir(&l);
}

static int cmd_verify(int argc, char **argv)
{
	if (argc < 1)
		return usage("verify needs MANIFEST");
	struct vstats st = { .data = true, .times = true };
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-data")) st.data = false;
		else if (!strcmp(argv[i], "--no-times")) st.times = false;
	}
	size_t mlen, eoff;
	char *text = json_read_file(argv[0], &mlen);
	if (!text)
		return fail(errno, "read %s", argv[0]);
	struct json_value *m = json_parse(text, mlen, &eoff);
	free(text);
	if (!m)
		return fail(0, "%s: JSON parse error at byte %zu", argv[0], eoff);
	struct json_value *entries = json_get(m, "entries");
	if (!entries || entries->type != JSON_ARRAY) {
		json_free(m);
		return fail(0, "%s: no entries array", argv[0]);
	}
	int rc = do_mount(true);
	if (rc) {
		json_free(m);
		return rc;
	}
	/* volume-level fields */
	struct json_value *vol = json_get(m, "volume");
	struct ntfs_volume_info vi;
	if (vol && !ntfs_volume_get_info(g.vol, &vi)) {
		if (vi.cluster_size != (uint32_t)json_get_num(vol, "cluster_size", 0))
			mismatch(&st, "(volume)", "cluster_size %u, manifest %.0f", vi.cluster_size, json_get_num(vol, "cluster_size", 0));
		if (strcmp(vi.label, json_get_str(vol, "label", "")))
			mismatch(&st, "(volume)", "label '%s', manifest '%s'", vi.label, json_get_str(vol, "label", ""));
		const char *sh = json_get_str(vol, "serial_hex", NULL);
		if (sh && vi.serial != strtoull(sh, NULL, 16))
			mismatch(&st, "(volume)", "serial %016" PRIx64 ", manifest %s", vi.serial, sh);
	}
	struct pathset ps = { .v = calloc(entries->u.arr.n, sizeof(char *)), .n = 0 };
	for (size_t i = 0; i < entries->u.arr.n; i++) {
		const char *p = json_get_str(entries->u.arr.items[i], "path", NULL);
		if (p)
			ps.v[ps.n++] = (char *)p;
		verify_entry(&st, entries->u.arr.items[i]);
	}
	qsort(ps.v, ps.n, sizeof(char *), pcmp);
	verify_extra(&st, &ps, g.root, "/");
	free(ps.v);
	do_unmount();
	printf("verify %s: %d entries checked, %d mismatches (%d missing, %d extra)%s%s\n", g.image,
	       st.checked, st.mismatched, st.missing, st.extra, st.data ? "" : " [no data]", st.times ? "" : " [no times]");
	json_free(m);
	return st.mismatched ? 3 : 0;
}

/* ---- bench --------------------------------------------------------------- */

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmd_bench(int argc, char **argv)
{
	uint64_t size_mib = 256;
	int nfiles = 1000;
	const char *dirpath = "/";
	for (int i = 0; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "--size")) size_mib = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--files")) nfiles = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--dir")) dirpath = argv[++i];
	}
	int rc = do_mount(false);
	if (rc)
		return rc;
	ntfs_inode_t *dir, *bd = NULL, *ni = NULL;
	int err = resolve(dirpath, &dir);
	if (err) {
		rc = fail(err, "lookup %s", dirpath);
		goto out;
	}
	err = ntfs_mkdir(dir, "ntfscli-bench", 0755, &bd);
	if (err) {
		rc = fail(err, "mkdir bench dir");
		goto out_dir;
	}
	uint8_t *buf = malloc(CHUNK);
	for (size_t i = 0; i < CHUNK; i++)
		buf[i] = (uint8_t)(i * 2654435761u >> 24);
	double t0, t1;

	/* sequential write */
	err = ntfs_create(bd, "big.bin", 0644, &ni);
	if (err) {
		rc = fail(err, "create big.bin");
		goto out_bd;
	}
	t0 = now_s();
	for (uint64_t off = 0; off < size_mib << 20 && !err; off += CHUNK) {
		ssize_t w = ntfs_write(ni, buf, CHUNK, off);
		if (w != (ssize_t)CHUNK)
			err = w < 0 ? (int)w : -EIO;
	}
	if (!err)
		err = ntfs_fsync(ni, false);
	t1 = now_s();
	if (err) {
		rc = fail(err, "sequential write");
		goto out_big;
	}
	printf("seq write %6" PRIu64 " MiB: %8.1f MiB/s\n", size_mib, (double)size_mib / (t1 - t0));

	/* sequential read */
	t0 = now_s();
	for (uint64_t off = 0; off < size_mib << 20 && !err; off += CHUNK) {
		ssize_t r = ntfs_read(ni, buf, CHUNK, off);
		if (r != (ssize_t)CHUNK)
			err = r < 0 ? (int)r : -EIO;
	}
	t1 = now_s();
	if (err) {
		rc = fail(err, "sequential read");
		goto out_big;
	}
	printf("seq read  %6" PRIu64 " MiB: %8.1f MiB/s\n", size_mib, (double)size_mib / (t1 - t0));
	ntfs_inode_put(ni);
	ni = NULL;
	err = ntfs_unlink(bd, "big.bin");

	/* create N small files */
	t0 = now_s();
	for (int i = 0; i < nfiles && !err; i++) {
		char name[32];
		snprintf(name, sizeof(name), "f%06d", i);
		ntfs_inode_t *f;
		err = ntfs_create(bd, name, 0644, &f);
		if (!err) {
			ssize_t w = ntfs_write(f, buf, 4096, 0);
			if (w != 4096)
				err = w < 0 ? (int)w : -EIO;
			ntfs_inode_put(f);
		}
	}
	if (!err)
		err = ntfs_volume_sync(g.vol);
	t1 = now_s();
	if (err) {
		rc = fail(err, "create files");
		goto out_bd;
	}
	printf("create %6d files (4 KiB each) + sync: %8.0f files/s\n", nfiles, nfiles / (t1 - t0));

	/* stat all */
	t0 = now_s();
	for (int i = 0; i < nfiles && !err; i++) {
		char name[32];
		snprintf(name, sizeof(name), "f%06d", i);
		ntfs_inode_t *f;
		struct ntfs_attr a;
		err = ntfs_lookup(bd, name, &f);
		if (!err) {
			err = ntfs_getattr(f, &a);
			ntfs_inode_put(f);
		}
	}
	t1 = now_s();
	if (err) {
		rc = fail(err, "stat files");
		goto out_bd;
	}
	printf("lookup+stat %6d files: %8.0f files/s\n", nfiles, nfiles / (t1 - t0));

	/* readdir */
	t0 = now_s();
	struct dlist l;
	err = read_dir(bd, true, &l);
	t1 = now_s();
	if (err) {
		rc = fail(err, "readdir");
		goto out_bd;
	}
	printf("readdir(+attr) %6zu entries: %8.0f entries/s\n", l.n, l.n / (t1 - t0));
	free_dir(&l);

	/* unlink all */
	t0 = now_s();
	for (int i = 0; i < nfiles && !err; i++) {
		char name[32];
		snprintf(name, sizeof(name), "f%06d", i);
		err = ntfs_unlink(bd, name);
	}
	if (!err)
		err = ntfs_volume_sync(g.vol);
	t1 = now_s();
	if (err) {
		rc = fail(err, "unlink files");
		goto out_bd;
	}
	printf("unlink %6d files + sync: %8.0f files/s\n", nfiles, nfiles / (t1 - t0));

out_big:
	if (ni)
		ntfs_inode_put(ni);
out_bd:
	free(buf);
	ntfs_inode_put(bd);
	if (!rc) {
		err = ntfs_rmdir(dir, "ntfscli-bench");
		if (err)
			rc = fail(err, "rmdir bench dir");
	}
out_dir:
	ntfs_inode_put(dir);
out:
	{
		int urc = do_unmount();
		return rc ? rc : urc;
	}
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--ro")) g.flags |= NTFS_MOUNT_RDONLY;
		else if (!strcmp(argv[i], "--case-sensitive")) g.flags |= NTFS_MOUNT_CASE_SENSITIVE;
		else if (!strcmp(argv[i], "--allow-illegal")) g.flags |= NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL;
		else if (!strcmp(argv[i], "--show-system")) g.flags |= NTFS_MOUNT_SHOW_SYSTEM;
		else if (!strcmp(argv[i], "--hide-hidden")) g.flags |= NTFS_MOUNT_HIDE_HIDDEN;
		else if (!strcmp(argv[i], "--no-fallback")) g.flags &= ~NTFS_MOUNT_RDONLY_FALLBACK;
		else if (!strcmp(argv[i], "--discard")) g.flags |= NTFS_MOUNT_DISCARD;
		else if (!strcmp(argv[i], "-v")) g.verbose++;
		else if (!strcmp(argv[i], "-vv")) g.verbose += 2;
		else if (!strcmp(argv[i], "-q")) g.verbose = -1;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) return usage(NULL);
		else return usage("unknown option");
	}
	if (argc - i < 2)
		return usage(NULL);
	g.cmd = argv[i];
	g.image = argv[i + 1];
	int cargc = argc - i - 2;
	char **cargv = argv + i + 2;
	ntfs_set_logger(logger, NULL);

	static const struct { const char *name; int (*fn)(int, char **); } cmds[] = {
		{ "probe", cmd_probe }, { "info", cmd_info }, { "ls", cmd_ls }, { "stat", cmd_stat },
		{ "cat", cmd_cat }, { "cp-out", cmd_cp_out }, { "cp-in", cmd_cp_in },
		{ "mkdir", cmd_simple }, { "rm", cmd_simple }, { "rmdir", cmd_simple }, { "mv", cmd_simple },
		{ "ln", cmd_simple }, { "truncate", cmd_simple }, { "xattr", cmd_xattr }, { "sync", cmd_sync },
		{ "verify", cmd_verify }, { "bench", cmd_bench },
	};
	for (size_t k = 0; k < sizeof(cmds) / sizeof(*cmds); k++)
		if (!strcmp(cmds[k].name, g.cmd))
			return cmds[k].fn(cargc, cargv);
	return usage("unknown command");
}
