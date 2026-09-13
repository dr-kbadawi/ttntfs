// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * api.c - the public C ABI (core/include/ntfscore.h) of the port.
 *
 * PORT: this is the replacement for the kernel VFS's syscall layer. Handles
 * are the port's struct inode; references map to ihold()/iput(). Names come
 * in as UTF-8 and go through unistr.c's converters. Locking follows the
 * kernel: i_rwsem of a directory exclusive for namespace changes and shared
 * for lookups/readdir; i_rwsem of a file exclusive for writes/truncates and
 * shared for reads; a per-volume mutex serializes renames.
 *
 * Data path (docs/PORTING.md §3, rule 3): a non-resident, non-compressed,
 * non-encrypted stream is read and written straight from the run list to
 * the device for whole PAGE_SIZE pages; only the unaligned edges, resident
 * data and compressed streams touch the metadata page cache, which is kept
 * coherent with the direct I/O by flushing/invalidating the affected pages.
 */

#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/uio.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/xattr.h>

/*
 * PORT: core/ntfs/inode.h declares an internal struct ntfs_attr and the
 * kernel-signature ntfs_getattr()/ntfs_setattr(); the public ABI in
 * ntfscore.h uses the same names. Rename the internal ones for this file.
 */
#define ntfs_attr ntfs_attr_internal
#define ntfs_getattr ntfs_getattr_internal
#define ntfs_setattr ntfs_setattr_internal
#include "ntfs.h"
#include "attrib.h"
#include "mft.h"
#include "dir.h"
#include "reparse.h"
#include "vfs.h"
#undef ntfs_attr
#undef ntfs_getattr
#undef ntfs_setattr
#include "glue.h"

/* Kept in step with super_glue.c's copy: the resumable-image header. */
#define NTFS_HIBERFIL_HEADER_SIZE 4096

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

#define VI(h) ((struct inode *)(h))
#define HANDLE(vi) ((ntfs_inode_t *)(vi))

/* Serializes renames, like the kernel's per-sb s_vfs_rename_mutex. */
static DEFINE_MUTEX(rename_mutex);

static inline struct ntfs_timespec ts_out(struct timespec64 t)
{
	return (struct ntfs_timespec){ .sec = t.tv_sec, .nsec = (int32_t)t.tv_nsec };
}

static inline struct timespec64 ts_in(struct ntfs_timespec t)
{
	return (struct timespec64){ .tv_sec = t.sec, .tv_nsec = t.nsec };
}

static inline void lock_two_dirs(struct inode *a, struct inode *b)
{
	if (a == b) {
		inode_lock(a);
	} else if (a->i_ino < b->i_ino) {
		inode_lock(a);
		inode_lock(b);
	} else {
		inode_lock(b);
		inode_lock(a);
	}
}

static inline void unlock_two_dirs(struct inode *a, struct inode *b)
{
	inode_unlock(a);
	if (a != b)
		inode_unlock(b);
}

/* ---- inode handles ---------------------------------------------------- */

int ntfs_inode_get(ntfs_volume_t *h, uint64_t inode_no, ntfs_inode_t **out)
{
	struct inode *vi;

	*out = NULL;
	if (!h)
		return -EINVAL;
	vi = ntfs_iget(h->sb, inode_no);
	if (IS_ERR(vi))
		return PTR_ERR(vi);
	/* Only file/directory inodes are handles; attribute inodes are
	 * internal, and a record that is not in use has no name. */
	if (NInoAttr(NTFS_I(vi)) || !vi->i_nlink) {
		iput(vi);
		return -ENOENT;
	}
	*out = HANDLE(vi);
	return 0;
}

void ntfs_inode_ref(ntfs_inode_t *ni)
{
	if (ni)
		ihold(VI(ni));
}

void ntfs_inode_put(ntfs_inode_t *ni)
{
	if (ni)
		iput(VI(ni));
}

uint64_t ntfs_inode_number(ntfs_inode_t *ni)
{
	return ni ? VI(ni)->i_ino : 0;
}

/* ---- attributes ------------------------------------------------------- */

static int item_type_of(struct inode *vi)
{
	struct ntfs_inode *ni = NTFS_I(vi);

	if (S_ISDIR(vi->i_mode))
		return NTFS_ITEM_DIR;
	if (S_ISLNK(vi->i_mode))
		return NTFS_ITEM_SYMLINK;
	if (S_ISREG(vi->i_mode) && !(ni->flags & FILE_ATTR_REPARSE_POINT))
		return NTFS_ITEM_FILE;
	return NTFS_ITEM_OTHER;
}

/*
 * noowners semantics (docs/PORTING.md §6): uid/gid are the mount's, the
 * permission bits come from the masks and the READONLY attribute.
 */
static uint32_t mode_of(struct inode *vi)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	umode_t type = vi->i_mode & S_IFMT;
	unsigned int perm;

	if (S_ISDIR(vi->i_mode))
		perm = 0777 & ~vol->dmask;
	else if (S_ISLNK(vi->i_mode))
		perm = 0777;
	else
		perm = 0777 & ~vol->fmask;
	/* Windows ignores READONLY on directories. */
	if ((ni->flags & FILE_ATTR_READONLY) && !S_ISDIR(vi->i_mode))
		perm &= ~0222;
	if (sb_rdonly(vi->i_sb))
		perm &= ~0222;
	return type | perm;
}

/* Walk the attributes once: named $DATA streams and the reparse tag. */
static void scan_attrs(struct inode *vi, bool *has_ads, uint32_t *reparse_tag)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_attr_search_ctx *ctx;

	*has_ads = false;
	*reparse_tag = 0;
	mutex_lock(&ni->mrec_lock);
	ctx = ntfs_attr_get_search_ctx(ni, NULL);
	if (ctx) {
		while (!ntfs_attrs_walk(ctx)) {
			struct attr_record *a = ctx->attr;

			if (a->type == AT_DATA && a->name_length)
				*has_ads = true;
			else if (a->type == AT_REPARSE_POINT && !a->non_resident &&
				 le32_to_cpu(a->data.resident.value_length) >= 4)
				*reparse_tag = le32_to_cpu(*(__le32 *)((u8 *)a +
					le16_to_cpu(a->data.resident.value_offset)));
		}
		ntfs_attr_put_search_ctx(ctx);
	}
	mutex_unlock(&ni->mrec_lock);
}

static void fill_attr(struct inode *vi, struct ntfs_attr *a)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	unsigned long flags;

	memset(a, 0, sizeof(*a));
	a->inode_no = vi->i_ino;
	a->generation = vi->i_generation;
	a->type = item_type_of(vi);
	a->mode = mode_of(vi);
	a->nlink = vi->i_nlink;
	a->uid = uid_valid(vol->uid) ? vol->uid.val : vi->i_uid.val;
	a->gid = gid_valid(vol->gid) ? vol->gid.val : vi->i_gid.val;
	read_lock_irqsave(&ni->size_lock, flags);
	if (S_ISLNK(vi->i_mode) && ni->target)
		a->size = strlen(ni->target);
	else
		a->size = i_size_read(vi);
	if (S_ISREG(vi->i_mode) && (NInoCompressed(ni) || NInoSparse(ni)))
		a->alloc_size = ni->itype.compressed.size;
	else
		a->alloc_size = ni->allocated_size;
	read_unlock_irqrestore(&ni->size_lock, flags);
	a->atime = ts_out(inode_get_atime(vi));
	a->mtime = ts_out(inode_get_mtime(vi));
	a->ctime = ts_out(inode_get_ctime(vi));
	a->crtime = ts_out(ni->i_crtime);
	a->file_attributes = le32_to_cpu(ni->flags);
	a->compressed = NInoCompressed(ni) && S_ISREG(vi->i_mode);
	a->sparse = NInoSparse(ni) && S_ISREG(vi->i_mode);
	a->encrypted = NInoEncrypted(ni) && S_ISREG(vi->i_mode);
	if (S_ISDIR(vi->i_mode)) {
		/* For a directory the flags mean "create children like this". */
		a->compressed = !!(ni->flags & FILE_ATTR_COMPRESSED);
		a->sparse = !!(ni->flags & FILE_ATTR_SPARSE_FILE);
		a->encrypted = !!(ni->flags & FILE_ATTR_ENCRYPTED);
	}
	scan_attrs(vi, &a->has_ads, &a->reparse_tag);
}

int ntfs_getattr(ntfs_inode_t *h, struct ntfs_attr *attr)
{
	if (!h || !attr)
		return -EINVAL;
	fill_attr(VI(h), attr);
	return 0;
}

int ntfs_setattr(ntfs_inode_t *h, const struct ntfs_attr *attr, uint32_t valid)
{
	struct inode *vi = VI(h);
	struct ntfs_inode *ni = NTFS_I(vi);
	struct iattr ia = { 0 };
	int err = 0;

	if (!h || !attr)
		return -EINVAL;
	if (IS_RDONLY(vi))
		return -EROFS;
	if ((valid & NTFS_SETATTR_SIZE) && S_ISDIR(vi->i_mode))
		return -EISDIR;

	inode_lock(vi);
	if (valid & NTFS_SETATTR_SIZE) {
		ia.ia_valid |= ATTR_SIZE;
		ia.ia_size = attr->size;
	}
	if (valid & NTFS_SETATTR_MODE) {
		ia.ia_valid |= ATTR_MODE;
		ia.ia_mode = attr->mode & ~S_IFMT;
	}
	/* NTFS_SETATTR_UID/GID: noowners, accepted and ignored. */
	if (valid & NTFS_SETATTR_ATIME) {
		ia.ia_valid |= ATTR_ATIME;
		ia.ia_atime = ts_in(attr->atime);
	}
	if (valid & NTFS_SETATTR_MTIME) {
		ia.ia_valid |= ATTR_MTIME;
		ia.ia_mtime = ts_in(attr->mtime);
	}
	if (valid & NTFS_SETATTR_CTIME) {
		ia.ia_valid |= ATTR_CTIME;
		ia.ia_ctime = ts_in(attr->ctime);
	}
	if (valid & NTFS_SETATTR_FLAGS) {
		__le32 settable = FILE_ATTR_VALID_SET_FLAGS;
		__le32 nflags = (ni->flags & ~settable) |
				(cpu_to_le32(attr->file_attributes) & settable);

		if (nflags != ni->flags) {
			ni->flags = nflags;
			NInoSetFileNameDirty(ni);
			if (!(ia.ia_valid & ATTR_CTIME)) {
				ia.ia_valid |= ATTR_CTIME;
				ia.ia_ctime = current_time(vi);
			}
		}
	}
	if (valid & NTFS_SETATTR_CRTIME) {
		ni->i_crtime = ts_in(attr->crtime);
		NInoSetFileNameDirty(ni);
		mark_inode_dirty(vi);
	}
	if (ia.ia_valid)
		err = ntfs_vfs_setattr(vi, &ia);
	inode_unlock(vi);
	return err;
}

/* ---- namespace -------------------------------------------------------- */

/* Path components must be plain names. */
static int check_component(const char *name, size_t *len_out)
{
	size_t len;

	if (!name)
		return -EINVAL;
	len = strlen(name);
	if (!len || len > NTFS_MAX_NAME_BYTES)
		return len ? -ENAMETOOLONG : -EINVAL;
	if (memchr(name, '/', len))
		return -EINVAL;
	if (name[0] == '.' && (len == 1 || (len == 2 && name[1] == '.')))
		return -EINVAL;
	*len_out = len;
	return 0;
}

int ntfs_lookup(ntfs_inode_t *dh, const char *name, ntfs_inode_t **out)
{
	struct inode *dir = VI(dh), *vi;
	size_t len;
	int err;

	*out = NULL;
	if (!dh || !name)
		return -EINVAL;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	len = strlen(name);
	if (!len || memchr(name, '/', len))
		return -EINVAL;
	if (name[0] == '.' && len == 1) {
		ihold(dir);
		*out = dh;
		return 0;
	}
	if (name[0] == '.' && len == 2 && name[1] == '.') {
		u64 parent = ntfs_vfs_parent_ino(dir);

		if (parent == (u64)-1)
			return -EIO;
		vi = ntfs_iget(dir->i_sb, parent);
		if (IS_ERR(vi))
			return PTR_ERR(vi);
		*out = HANDLE(vi);
		return 0;
	}
	if (len > NTFS_MAX_NAME_BYTES)
		return -ENAMETOOLONG;
	inode_lock_shared(dir);
	vi = ntfs_vfs_lookup(dir, name, (int)len);
	inode_unlock_shared(dir);
	if (IS_ERR(vi))
		return PTR_ERR(vi);
	err = 0;
	*out = HANDLE(vi);
	return err;
}

static int do_create(ntfs_inode_t *dh, const char *name, umode_t mode,
		     const char *target, ntfs_inode_t **out)
{
	struct inode *dir = VI(dh), *vi;
	size_t len;
	int err;

	if (out)
		*out = NULL;
	if (!dh)
		return -EINVAL;
	err = check_component(name, &len);
	if (err)
		return err;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	if (IS_RDONLY(dir))
		return -EROFS;
	inode_lock(dir);
	vi = ntfs_vfs_lookup(dir, name, (int)len);
	if (!IS_ERR(vi)) {
		iput(vi);
		err = -EEXIST;
	} else if (PTR_ERR(vi) != -ENOENT) {
		err = PTR_ERR(vi);
	} else {
		vi = ntfs_vfs_create(dir, name, (int)len, mode, target);
		if (IS_ERR(vi))
			err = PTR_ERR(vi);
		else if (out)
			*out = HANDLE(vi);
		else
			iput(vi);
	}
	inode_unlock(dir);
	return err;
}

int ntfs_create(ntfs_inode_t *dir, const char *name, uint32_t mode, ntfs_inode_t **out)
{
	return do_create(dir, name, S_IFREG | (mode & 0777), NULL, out);
}

int ntfs_mkdir(ntfs_inode_t *dir, const char *name, uint32_t mode, ntfs_inode_t **out)
{
	return do_create(dir, name, S_IFDIR | (mode & 0777), NULL, out);
}

int ntfs_symlink(ntfs_inode_t *dir, const char *name, const char *target,
		 ntfs_inode_t **out)
{
	if (!target || !*target)
		return -EINVAL;
	if (strlen(target) >= PATH_MAX)
		return -ENAMETOOLONG;
	return do_create(dir, name, S_IFLNK | 0777, target, out);
}

/* Windows (IO_REPARSE_TAG_SYMLINK) reparse data. */
struct ntfs_win_symlink {
	__le16 subst_name_offset;
	__le16 subst_name_length;
	__le16 print_name_offset;
	__le16 print_name_length;
	__le32 flags;
	__le16 path_buffer[];
} __packed;

int ntfs_readlink(ntfs_inode_t *h, char *buf, size_t bufsize, size_t *len_out)
{
	struct inode *vi = VI(h);
	struct ntfs_inode *ni = NTFS_I(vi);
	struct reparse_point *rp = NULL;
	s64 rp_size = 0;
	int err = -EINVAL;

	if (!h || !len_out)
		return -EINVAL;
	*len_out = 0;
	if (!S_ISLNK(vi->i_mode))
		return -EINVAL;

	if (ni->target) {
		/* WSL symlink, decoded by reparse.c at inode load. */
		*len_out = strlen(ni->target);
		if (buf) {
			if (bufsize < *len_out)
				return -ERANGE;
			memcpy(buf, ni->target, *len_out);
		}
		return 0;
	}

	mutex_lock(&ni->mrec_lock);
	rp = ntfs_attr_readall(ni, AT_REPARSE_POINT, NULL, 0, &rp_size);
	mutex_unlock(&ni->mrec_lock);
	if (rp && rp_size >= (s64)sizeof(*rp) &&
	    rp->reparse_tag == IO_REPARSE_TAG_SYMLINK) {
		struct ntfs_win_symlink *sl = (struct ntfs_win_symlink *)rp->reparse_data;
		unsigned int off = le16_to_cpu(sl->print_name_offset);
		unsigned int nlen = le16_to_cpu(sl->print_name_length);
		unsigned char *s = NULL;
		int slen;

		if (!nlen) {
			off = le16_to_cpu(sl->subst_name_offset);
			nlen = le16_to_cpu(sl->subst_name_length);
		}
		if (sizeof(*sl) + off + nlen > le16_to_cpu(rp->reparse_data_length))
			goto out;
		slen = ntfs_ucstonls(ni->vol, (const __le16 *)((u8 *)sl->path_buffer + off),
				     nlen / 2, &s, 0);
		if (slen < 0) {
			err = slen;
			goto out;
		}
		{
			char *p = (char *)s, *q = p;

			/* "\??\" and "X:" prefixes have no meaning here. */
			if (slen >= 4 && !memcmp(p, "\\??\\", 4))
				p += 4;
			if (isalpha((unsigned char)p[0]) && p[1] == ':')
				p += 2;
			for (q = p; *q; q++)
				if (*q == '\\')
					*q = '/';
			*len_out = strlen(p);
			err = 0;
			if (buf) {
				if (bufsize < *len_out)
					err = -ERANGE;
				else
					memcpy(buf, p, *len_out);
			}
		}
		kfree(s);
	}
out:
	kvfree(rp);
	return err;
}

int ntfs_link(ntfs_inode_t *h, ntfs_inode_t *dh, const char *name)
{
	struct inode *vi = VI(h), *dir = VI(dh), *old;
	size_t len;
	int err;

	if (!h || !dh)
		return -EINVAL;
	err = check_component(name, &len);
	if (err)
		return err;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	if (S_ISDIR(vi->i_mode))
		return -EPERM;
	if (IS_RDONLY(dir))
		return -EROFS;
	inode_lock(dir);
	old = ntfs_vfs_lookup(dir, name, (int)len);
	if (!IS_ERR(old)) {
		iput(old);
		err = -EEXIST;
	} else if (PTR_ERR(old) != -ENOENT) {
		err = PTR_ERR(old);
	} else {
		err = ntfs_vfs_link(vi, dir, name, (int)len);
	}
	inode_unlock(dir);
	return err;
}

static int do_unlink(ntfs_inode_t *dh, const char *name, bool rmdir)
{
	struct inode *dir = VI(dh);
	size_t len;
	int err;

	if (!dh)
		return -EINVAL;
	err = check_component(name, &len);
	if (err)
		return err;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	if (IS_RDONLY(dir))
		return -EROFS;
	inode_lock(dir);
	err = ntfs_vfs_unlink(dir, name, (int)len, rmdir);
	inode_unlock(dir);
	return err;
}

int ntfs_unlink(ntfs_inode_t *dir, const char *name)
{
	return do_unlink(dir, name, false);
}

int ntfs_rmdir(ntfs_inode_t *dir, const char *name)
{
	return do_unlink(dir, name, true);
}

/* Is @ancestor_ino an ancestor of (or equal to) directory @dir? */
static int is_subdir(struct inode *dir, u64 ancestor_ino)
{
	u64 cur = dir->i_ino;
	int depth = 0;

	while (depth++ < 4096) {
		struct inode *vi;
		u64 parent;

		if (cur == ancestor_ino)
			return 1;
		if (cur == FILE_root)
			return 0;
		vi = ntfs_iget(dir->i_sb, cur);
		if (IS_ERR(vi))
			return PTR_ERR(vi);
		parent = ntfs_vfs_parent_ino(vi);
		iput(vi);
		if (parent == (u64)-1)
			return -EIO;
		if (parent == cur)
			return 0;
		cur = parent;
	}
	return -ELOOP;
}

int ntfs_rename(ntfs_inode_t *odh, const char *old_name,
		ntfs_inode_t *ndh, const char *new_name)
{
	struct inode *old_dir = VI(odh), *new_dir = VI(ndh);
	size_t old_len, new_len;
	int err;

	if (!odh || !ndh)
		return -EINVAL;
	err = check_component(old_name, &old_len);
	if (err)
		return err;
	err = check_component(new_name, &new_len);
	if (err)
		return err;
	if (!S_ISDIR(old_dir->i_mode) || !S_ISDIR(new_dir->i_mode))
		return -ENOTDIR;
	if (old_dir->i_sb != new_dir->i_sb)
		return -EXDEV;
	if (IS_RDONLY(old_dir))
		return -EROFS;

	mutex_lock(&rename_mutex);
	lock_two_dirs(old_dir, new_dir);

	if (old_dir != new_dir) {
		/* A directory must not be moved into its own subtree. */
		struct inode *vi = ntfs_vfs_lookup(old_dir, old_name, (int)old_len);

		if (IS_ERR(vi)) {
			err = PTR_ERR(vi);
			goto out;
		}
		if (S_ISDIR(vi->i_mode)) {
			int r = is_subdir(new_dir, vi->i_ino);

			if (r < 0)
				err = r;
			else if (r)
				err = -EINVAL;
		}
		iput(vi);
		if (err)
			goto out;
	}
	err = ntfs_vfs_rename(old_dir, old_name, (int)old_len,
			      new_dir, new_name, (int)new_len);
out:
	unlock_two_dirs(old_dir, new_dir);
	mutex_unlock(&rename_mutex);
	return err;
}

/* ---- readdir ---------------------------------------------------------- */

struct readdir_ctx {
	struct dir_context ctx;		/* must be first */
	struct inode *dir;
	bool want_attr;
	ntfs_readdir_cb cb;
	void *cb_ctx;
	bool stopped;
	int err;
};

static bool readdir_actor(struct dir_context *dctx, const char *name, int namelen,
			  loff_t pos, u64 ino, unsigned type)
{
	struct readdir_ctx *rc = (struct readdir_ctx *)dctx;
	struct ntfs_dirent ent;
	int rc_cb;

	memset(&ent, 0, sizeof(ent));
	ent.inode_no = ino;
	ent.name = name;
	ent.name_len = namelen;
	ent.cookie = pos;
	switch (type) {
	case DT_DIR:
		ent.type = NTFS_ITEM_DIR;
		break;
	case DT_LNK:
		ent.type = NTFS_ITEM_SYMLINK;
		break;
	case DT_REG:
		ent.type = NTFS_ITEM_FILE;
		break;
	default:
		ent.type = NTFS_ITEM_OTHER;
		break;
	}
	if (rc->want_attr) {
		struct inode *vi;

		if (ino == rc->dir->i_ino) {
			vi = rc->dir;
			ihold(vi);
		} else {
			vi = ntfs_iget(rc->dir->i_sb, ino);
		}
		if (!IS_ERR(vi)) {
			fill_attr(vi, &ent.attr);
			ent.has_attr = true;
			/* The index knows better than the mode for reparse
			 * points; keep the type readdir derived. */
			iput(vi);
		}
	}
	rc_cb = rc->cb(&ent, rc->cb_ctx);
	if (rc_cb) {
		rc->stopped = true;
		return false;
	}
	return true;
}

int ntfs_readdir(ntfs_inode_t *dh, uint64_t *cookie, bool want_attr,
		 ntfs_readdir_cb cb, void *ctx, bool *eof)
{
	struct inode *dir = VI(dh);
	struct readdir_ctx rc;
	struct file file;
	int err;

	if (!dh || !cookie || !cb || !eof)
		return -EINVAL;
	*eof = false;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	if (!dir->i_fop || !dir->i_fop->iterate_shared)
		return -EIO;

	memset(&rc, 0, sizeof(rc));
	rc.ctx.actor = readdir_actor;
	rc.ctx.pos = (loff_t)*cookie;
	rc.dir = dir;
	rc.want_attr = want_attr;
	rc.cb = cb;
	rc.cb_ctx = ctx;

	memset(&file, 0, sizeof(file));
	file.f_inode = dir;
	file.f_mapping = dir->i_mapping;
	file.f_pos = rc.ctx.pos;

	inode_lock_shared(dir);
	err = dir->i_fop->iterate_shared(&file, &rc.ctx);
	if (dir->i_fop->release)
		dir->i_fop->release(dir, &file);
	inode_unlock_shared(dir);
	if (err)
		return err;
	*cookie = (uint64_t)rc.ctx.pos;
	*eof = !rc.stopped;
	return 0;
}

/* ---- data ------------------------------------------------------------- */

/* Copy [pos, pos + len) of a stream through the page cache. */
static int cached_read(struct inode *vi, void *buf, loff_t pos, size_t len)
{
	struct address_space *mapping = vi->i_mapping;

	while (len) {
		struct folio *folio;
		size_t off = pos & ~PAGE_MASK;
		size_t n = min_t(size_t, PAGE_SIZE - off, len);

		folio = read_mapping_folio(mapping, pos >> PAGE_SHIFT, NULL);
		if (IS_ERR(folio))
			return PTR_ERR(folio);
		memcpy_from_folio(buf, folio, off, n);
		folio_put(folio);
		buf = (u8 *)buf + n;
		pos += n;
		len -= n;
	}
	return 0;
}

static int cached_write(struct inode *vi, const void *buf, loff_t pos, size_t len)
{
	struct address_space *mapping = vi->i_mapping;

	while (len) {
		struct folio *folio;
		size_t off = pos & ~PAGE_MASK;
		size_t n = min_t(size_t, PAGE_SIZE - off, len);

		folio = read_mapping_folio(mapping, pos >> PAGE_SHIFT, NULL);
		if (IS_ERR(folio))
			return PTR_ERR(folio);
		folio_lock(folio);
		memcpy_to_folio(folio, off, buf, n);
		folio_mark_dirty(folio);
		folio_unlock(folio);
		folio_put(folio);
		buf = (const u8 *)buf + n;
		pos += n;
		len -= n;
	}
	return 0;
}

/* Bring the device up to date with the cache for [pos, end) and drop the
 * cached copies, so a direct transfer sees and leaves the truth. */
static void cache_sync_range(struct inode *vi, loff_t pos, loff_t end)
{
	struct address_space *mapping = vi->i_mapping;

	if (!mapping->nrpages)
		return;
	filemap_write_and_wait_range(mapping, pos, end - 1);
	invalidate_inode_pages2_range(mapping, pos >> PAGE_SHIFT,
				      (end - 1) >> PAGE_SHIFT);
}

static inline struct ntfs_inode *base_of(struct ntfs_inode *ni)
{
	return NInoAttr(ni) ? ni->ext.base_ntfs_ino : ni;
}

static ssize_t resident_read(struct inode *vi, void *buf, size_t count, loff_t pos)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_attr_search_ctx *ctx;
	u32 attr_len;
	ssize_t ret;

	mutex_lock(&ni->mrec_lock);
	ctx = ntfs_attr_get_search_ctx(base_of(ni), NULL);
	if (!ctx) {
		ret = -ENOMEM;
		goto out;
	}
	ret = ntfs_attr_lookup(ni->type, ni->name, ni->name_len, CASE_SENSITIVE,
			       0, NULL, 0, ctx);
	if (ret) {
		if (ret == -ENOENT)
			ret = -EIO;
		goto put;
	}
	if (ctx->attr->non_resident) {
		ret = -EAGAIN;	/* converted under us; caller retries */
		goto put;
	}
	attr_len = le32_to_cpu(ctx->attr->data.resident.value_length);
	if (pos >= attr_len) {
		ret = 0;
		goto put;
	}
	ret = min_t(size_t, count, attr_len - pos);
	memcpy(buf, (u8 *)ctx->attr +
	       le16_to_cpu(ctx->attr->data.resident.value_offset) + pos, ret);
put:
	ntfs_attr_put_search_ctx(ctx);
out:
	mutex_unlock(&ni->mrec_lock);
	return ret;
}

/*
 * do_read - read [offset, offset + count) of the stream behind @vi
 *
 * Works for file inodes and for attribute inodes (named $DATA streams).
 * Locking: caller holds vi's i_rwsem shared (or exclusive).
 */
static ssize_t do_read(struct inode *vi, void *buf, size_t count, loff_t pos)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	loff_t i_size, init_size, end, data_end;
	unsigned long flags;
	ssize_t ret;
	int err;

	i_size = i_size_read(vi);
	if (pos >= i_size) {
		ret = 0;
		goto out;
	}
	if (count > (size_t)(i_size - pos))
		count = i_size - pos;
	end = pos + count;

	if (NInoEncrypted(ni)) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (!NInoNonResident(ni)) {
		ret = resident_read(vi, buf, count, pos);
		if (ret != -EAGAIN)
			goto out;
	}
	if (NInoCompressed(ni)) {
		err = cached_read(vi, buf, pos, count);
		ret = err ? err : (ssize_t)count;
		goto out;
	}

	read_lock_irqsave(&ni->size_lock, flags);
	init_size = ni->initialized_size;
	read_unlock_irqrestore(&ni->size_lock, flags);
	data_end = min(end, init_size);
	if (data_end > pos) {
		loff_t head_end = min(data_end, (loff_t)round_up(pos, PAGE_SIZE));
		loff_t mid_end = max(head_end, (loff_t)round_down(data_end, PAGE_SIZE));
		u8 *p = buf;

		err = cached_read(vi, p, pos, head_end - pos);
		if (err)
			goto err;
		p += head_end - pos;
		if (mid_end > head_end) {
			cache_sync_range(vi, head_end, mid_end);
			err = ntfs_vfs_direct_read(vi, p, head_end, mid_end - head_end);
			if (err)
				goto err;
			p += mid_end - head_end;
		}
		if (data_end > mid_end) {
			err = cached_read(vi, p, mid_end, data_end - mid_end);
			if (err)
				goto err;
		}
	} else {
		data_end = pos;
	}
	if (end > data_end)
		memset((u8 *)buf + (data_end - pos), 0, end - data_end);
	ret = count;
	goto out;
err:
	ret = err;
out:
	return ret;
}

ssize_t ntfs_read(ntfs_inode_t *h, void *buf, size_t count, uint64_t offset)
{
	struct inode *vi = VI(h);
	ssize_t ret;

	if (!h || (!buf && count))
		return -EINVAL;
	if (S_ISDIR(vi->i_mode))
		return -EISDIR;
	if (NVolShutdown(NTFS_I(vi)->vol))
		return -EIO;
	if (!count)
		return 0;
	inode_lock_shared(vi);
	ret = do_read(vi, buf, count, offset);
	inode_unlock_shared(vi);
	return ret;
}

/* Allocate real clusters under [pos, pos + len) (upstream's writeback
 * allocation, done up front because the port writes to the device
 * directly). */
static int alloc_range(struct ntfs_inode *ni, loff_t pos, size_t len)
{
	struct ntfs_volume *vol = ni->vol;
	s64 vcn = ntfs_bytes_to_cluster(vol, pos);
	s64 end_vcn = ntfs_bytes_to_cluster(vol, pos + len - 1) + 1;
	int err = 0;

	while (vcn < end_vcn) {
		s64 lcn, count;
		bool balloc;

		mutex_lock(&ni->mrec_lock);
		down_write(&ni->runlist.lock);
		err = ntfs_attr_map_cluster(ni, vcn, &lcn, &count, end_vcn - vcn,
					    &balloc, true, false);
		up_write(&ni->runlist.lock);
		mutex_unlock(&ni->mrec_lock);
		if (err)
			break;
		if (count <= 0) {
			err = -EIO;
			break;
		}
		vcn += count;
	}
	return err;
}

/*
 * ntfs_attr_set_initialized_size() opens its search context on the inode
 * it is given; for an attribute inode that maps a second, stale copy of the
 * base record (the kernel only calls it for base inodes). Same body, on the
 * base inode's record.
 */
static int set_initialized_size(struct ntfs_inode *ni, loff_t new_size)
{
	struct ntfs_attr_search_ctx *ctx;
	int err;

	if (!NInoNonResident(ni))
		return -EINVAL;
	ctx = ntfs_attr_get_search_ctx(base_of(ni), NULL);
	if (!ctx)
		return -ENOMEM;
	err = ntfs_attr_lookup(ni->type, ni->name, ni->name_len,
			       CASE_SENSITIVE, 0, NULL, 0, ctx);
	if (!err) {
		ctx->attr->data.non_resident.initialized_size = cpu_to_le64(new_size);
		ni->initialized_size = new_size;
		mark_mft_record_dirty(ctx->ntfs_ino);
	}
	ntfs_attr_put_search_ctx(ctx);
	return err;
}

static ssize_t resident_write(struct inode *vi, const void *buf, size_t count,
			      loff_t pos)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_attr_search_ctx *ctx;
	u32 attr_len;
	ssize_t ret;

	mutex_lock(&ni->mrec_lock);
	ctx = ntfs_attr_get_search_ctx(base_of(ni), NULL);
	if (!ctx) {
		ret = -ENOMEM;
		goto out;
	}
	ret = ntfs_attr_lookup(ni->type, ni->name, ni->name_len, CASE_SENSITIVE,
			       0, NULL, 0, ctx);
	if (ret) {
		if (ret == -ENOENT)
			ret = -EIO;
		goto put;
	}
	if (ctx->attr->non_resident) {
		ret = -EAGAIN;
		goto put;
	}
	attr_len = le32_to_cpu(ctx->attr->data.resident.value_length);
	if (pos + count > attr_len) {
		ret = -EIO;
		goto put;
	}
	memcpy((u8 *)ctx->attr + le16_to_cpu(ctx->attr->data.resident.value_offset) + pos,
	       buf, count);
	mark_mft_record_dirty(ctx->ntfs_ino);
	ret = count;
put:
	ntfs_attr_put_search_ctx(ctx);
out:
	mutex_unlock(&ni->mrec_lock);
	/* The cached view of the value (folio 0) is stale now. */
	if (ret > 0)
		invalidate_inode_pages2_range(vi->i_mapping, 0, 0);
	return ret;
}

/*
 * do_write - write [pos, pos + count) to the stream behind @vi
 *
 * Grows the attribute, allocates clusters under the range (the core's own
 * expansion leaves holes, and write_folio never allocates), zeroes the gap
 * up to the old initialized_size, writes (edges cached, whole pages direct)
 * and extends initialized_size. Works for file and attribute inodes.
 * Locking: caller holds vi's i_rwsem exclusive.
 */
static ssize_t do_write(struct inode *vi, const void *buf, size_t count, loff_t pos)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	loff_t end = pos + count, old_data_size, old_init_size, init_size;
	unsigned long flags;
	ssize_t ret;
	int err;

	old_data_size = ni->data_size;
	old_init_size = ni->initialized_size;

	if (NInoNonResident(ni) && NInoCompressed(ni)) {
		struct iov_iter from;

		iov_iter_init_src(&from, buf, count);
		ret = ntfs_compress_write(ni, pos, count, &from);
		goto done;
	}

	/* Grow the attribute (may turn a resident one non-resident). */
	if (end > ni->data_size) {
		mutex_lock(&ni->mrec_lock);
		err = ntfs_attr_expand(ni, end, 0);
		mutex_unlock(&ni->mrec_lock);
		if (err) {
			ret = err;
			goto done;
		}
	}

	if (!NInoNonResident(ni)) {
		ret = resident_write(vi, buf, count, pos);
		if (ret != -EAGAIN)
			goto done;
	}

	/* Non-resident: allocate, zero the gap up to pos, write, extend. */
	err = alloc_range(ni, pos, count);
	if (err) {
		ret = err;
		goto done;
	}
	read_lock_irqsave(&ni->size_lock, flags);
	init_size = ni->initialized_size;
	read_unlock_irqrestore(&ni->size_lock, flags);
	if (pos > init_size) {
		err = ntfs_vfs_zero_range(vi, init_size, pos - init_size);
		if (err) {
			ret = err;
			goto done;
		}
	}
	{
		loff_t head_end = min(end, (loff_t)round_up(pos, PAGE_SIZE));
		loff_t mid_end = max(head_end, (loff_t)round_down(end, PAGE_SIZE));
		const u8 *p = buf;

		err = cached_write(vi, p, pos, head_end - pos);
		if (err) {
			ret = err;
			goto done;
		}
		p += head_end - pos;
		if (mid_end > head_end) {
			cache_sync_range(vi, head_end, mid_end);
			err = ntfs_vfs_direct_write(vi, p, head_end, mid_end - head_end);
			if (err) {
				ret = err;
				goto done;
			}
			p += mid_end - head_end;
		}
		if (end > mid_end) {
			err = cached_write(vi, p, mid_end, end - mid_end);
			if (err) {
				ret = err;
				goto done;
			}
		}
	}
	if (end > init_size) {
		mutex_lock(&ni->mrec_lock);
		err = set_initialized_size(ni, end);
		mutex_unlock(&ni->mrec_lock);
		if (err) {
			ret = err;
			goto done;
		}
	}
	ret = count;
done:
	if (ret < 0) {
		/* Undo a size change of a failed write (upstream write_iter). */
		if (ni->initialized_size != old_init_size && NInoNonResident(ni)) {
			mutex_lock(&ni->mrec_lock);
			set_initialized_size(ni, old_init_size);
			mutex_unlock(&ni->mrec_lock);
		}
		if (ni->data_size != old_data_size) {
			truncate_setsize(vi, old_data_size);
			ntfs_attr_truncate(ni, old_data_size);
		}
	} else if (ret > 0) {
		inode_set_mtime_to_ts(vi, inode_set_ctime_current(vi));
		NInoSetFileNameDirty(ni);
		mark_inode_dirty(vi);
	}
	return ret;
}

/*
 * ntfs_glue_zero_hiberfil - discard the saved Windows hibernation image
 *
 * Windows' Fast Startup does not shut down: it saves the kernel session into
 * hiberfil.sys and resumes from it next boot, which includes its own cached
 * picture of this filesystem. Writing to the volume behind that would corrupt
 * it when Windows resumes, so a hibernated volume mounts read-only.
 *
 * Zeroing the header is exactly what Windows itself does once it has consumed
 * the image, and what super_glue.c's ntfs_glue_hibernated() looks for: the
 * saved session stops being resumable, Windows boots normally instead, and the
 * volume is safe to write. The filesystem is not touched -- only the header of
 * one file -- and unlike replaying a journal there is no inferred on-disk
 * layout involved. What it destroys is whatever the user had open in Windows.
 *
 * The volume must already be read-write; the caller does that, and only for a
 * volume that is otherwise clean.
 */
int ntfs_glue_zero_hiberfil(struct ntfs_volume *vol)
{
	struct inode *vi;
	void *zeros;
	ssize_t written;
	int err = 0;

	if (!vol || !vol->root_ino)
		return -EINVAL;
	inode_lock_shared(vol->root_ino);
	vi = ntfs_vfs_lookup(vol->root_ino, "hiberfil.sys", 12);
	inode_unlock_shared(vol->root_ino);
	if (IS_ERR(vi))
		return PTR_ERR(vi) == -ENOENT ? 0 : PTR_ERR(vi);
	if (IS_RDONLY(vi)) {
		err = -EROFS;
		goto out;
	}
	/* Only the header identifies a resumable image; leave the rest alone so
	 * the file keeps its size and Windows reuses it. */
	zeros = kzalloc(NTFS_HIBERFIL_HEADER_SIZE, GFP_KERNEL);
	if (!zeros) {
		err = -ENOMEM;
		goto out;
	}
	inode_lock(vi);
	written = do_write(vi, zeros, NTFS_HIBERFIL_HEADER_SIZE, 0);
	inode_unlock(vi);
	kfree(zeros);
	if (written < 0)
		err = (int)written;
	else if (written != NTFS_HIBERFIL_HEADER_SIZE)
		err = -EIO;
out:
	iput(vi);
	return err;
}

ssize_t ntfs_write(ntfs_inode_t *h, const void *buf, size_t count, uint64_t offset)
{
	struct inode *vi = VI(h);
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	ssize_t ret;

	if (!h || (!buf && count))
		return -EINVAL;
	if (S_ISDIR(vi->i_mode))
		return -EISDIR;
	if (NVolShutdown(vol))
		return -EIO;
	if (IS_RDONLY(vi))
		return -EROFS;
	if (NInoEncrypted(ni))
		return -EOPNOTSUPP;
	if (!count)
		return 0;
	if (offset + count > (u64)vi->i_sb->s_maxbytes)
		return -EFBIG;

	inode_lock(vi);
	if (!(vol->vol_flags & VOLUME_IS_DIRTY))
		ntfs_set_volume_flags(vol, VOLUME_IS_DIRTY);
	ret = do_write(vi, buf, count, offset);
	inode_unlock(vi);
	return ret;
}

int ntfs_truncate(ntfs_inode_t *h, uint64_t size)
{
	struct ntfs_attr a = { .size = size };

	return ntfs_setattr(h, &a, NTFS_SETATTR_SIZE);
}

int ntfs_fallocate(ntfs_inode_t *h, uint64_t offset, uint64_t len, bool keep_size)
{
	struct inode *vi = VI(h);
	int err;

	if (!h)
		return -EINVAL;
	if (!len)
		return 0;
	if (offset + len > (u64)vi->i_sb->s_maxbytes)
		return -EFBIG;
	inode_lock(vi);
	err = ntfs_vfs_fallocate(vi, keep_size ? FALLOC_FL_KEEP_SIZE : 0,
				 offset, len);
	inode_unlock(vi);
	return err;
}

int ntfs_fsync(ntfs_inode_t *h, bool datasync)
{
	struct inode *vi = VI(h);
	int err;

	if (!h)
		return -EINVAL;
	if (IS_RDONLY(vi))
		return 0;
	inode_lock(vi);
	err = ntfs_vfs_fsync(vi, datasync);
	inode_unlock(vi);
	return err;
}

int ntfs_seek_data_hole(ntfs_inode_t *h, uint64_t offset, bool want_hole,
			uint64_t *result)
{
	struct inode *vi = VI(h);
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	loff_t i_size, pos = offset;
	int err = 0;

	if (!h || !result)
		return -EINVAL;
	inode_lock_shared(vi);
	i_size = i_size_read(vi);
	if (pos >= i_size) {
		err = -ENXIO;
		goto out;
	}
	if (!NInoNonResident(ni) || NInoCompressed(ni)) {
		/* All data, then the hole at EOF. */
		*result = want_hole ? i_size : pos;
		goto out;
	}
	while (pos < i_size) {
		s64 vcn = ntfs_bytes_to_cluster(vol, pos), lcn;
		struct runlist_element *rl;
		s64 run_end;

		down_read(&ni->runlist.lock);
		lcn = ntfs_attr_vcn_to_lcn_nolock(ni, vcn, false);
		run_end = vcn + 1;
		if (lcn >= LCN_HOLE) {
			rl = __ntfs_attr_find_vcn_nolock(&ni->runlist, vcn);
			if (!IS_ERR(rl))
				run_end = rl->vcn + rl->length;
		}
		up_read(&ni->runlist.lock);
		if (lcn < LCN_HOLE && lcn != LCN_ENOENT) {
			err = -EIO;
			goto out;
		}
		if ((lcn >= 0 || lcn == LCN_DELALLOC) != want_hole) {
			*result = pos;
			goto out;
		}
		if (lcn == LCN_ENOENT)
			break;
		pos = ntfs_cluster_to_bytes(vol, run_end);
	}
	if (want_hole)
		*result = i_size;
	else
		err = -ENXIO;
out:
	inode_unlock_shared(vi);
	return err;
}

/* ---- extended attributes: alternate data streams --------------------- */

static int xattr_uname(struct ntfs_volume *vol, const char *name, __le16 **uname)
{
	size_t len;

	if (!name)
		return -EINVAL;
	len = strlen(name);
	if (!len)
		return -EINVAL;
	if (len > NTFS_MAX_NAME_BYTES)
		return -ENAMETOOLONG;
	return ntfs_nlstoucs(vol, name, (int)len, uname, NTFS_MAX_NAME_LEN);
}

int ntfs_getxattr(ntfs_inode_t *h, const char *name, void *buf, size_t size,
		  size_t *len_out)
{
	struct inode *vi = VI(h), *avi;
	struct ntfs_volume *vol = NTFS_I(vi)->vol;
	__le16 *uname;
	int ulen, err = 0;
	s64 n;

	if (!h || !len_out)
		return -EINVAL;
	*len_out = 0;
	ulen = xattr_uname(vol, name, &uname);
	if (ulen < 0)
		return ulen;
	inode_lock_shared(vi);
	avi = ntfs_attr_iget(vi, AT_DATA, uname, ulen);
	if (IS_ERR(avi)) {
		err = PTR_ERR(avi) == -ENOENT ? -ENOATTR : PTR_ERR(avi);
		goto out;
	}
	*len_out = i_size_read(avi);
	if (buf) {
		if (size < *len_out) {
			err = -ERANGE;
		} else if (*len_out) {
			inode_lock_shared(avi);
			n = do_read(avi, buf, *len_out, 0);
			inode_unlock_shared(avi);
			if (n < 0)
				err = (int)n;
			else if (n != (s64)*len_out)
				err = -EIO;
		}
	}
	iput(avi);
out:
	inode_unlock_shared(vi);
	kmem_cache_free(ntfs_name_cache, uname);
	return err;
}

int ntfs_setxattr(ntfs_inode_t *h, const char *name, const void *buf,
		  size_t size, int flags)
{
	struct inode *vi = VI(h), *avi;
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	__le16 *uname;
	int ulen, err = 0;

	if (!h || (!buf && size))
		return -EINVAL;
	if (IS_RDONLY(vi))
		return -EROFS;
	if (NVolShutdown(vol))
		return -EIO;
	ulen = xattr_uname(vol, name, &uname);
	if (ulen < 0)
		return ulen;
	err = ntfs_vfs_validate_uname(vol, uname, ulen);
	if (err)
		goto free;

	inode_lock(vi);
	if (!(vol->vol_flags & VOLUME_IS_DIRTY))
		ntfs_set_volume_flags(vol, VOLUME_IS_DIRTY);

	avi = ntfs_attr_iget(vi, AT_DATA, uname, ulen);
	if (!IS_ERR(avi)) {
		struct ntfs_inode *ani = NTFS_I(avi);

		if (flags & XATTR_CREATE) {
			err = -EEXIST;
		} else {
			/* Replace the value in place: truncate, then write. */
			inode_lock(avi);
			truncate_inode_pages(avi->i_mapping, 0);
			mutex_lock(&ani->mrec_lock);
			err = ntfs_attr_truncate(ani, 0);
			mutex_unlock(&ani->mrec_lock);
			if (!err)
				i_size_write(avi, 0);
			if (!err && size) {
				s64 n = do_write(avi, buf, size, 0);

				if (n < 0)
					err = (int)n;
				else if (n != (s64)size)
					err = -EIO;
			}
			inode_unlock(avi);
		}
		iput(avi);
	} else if (PTR_ERR(avi) != -ENOENT) {
		err = PTR_ERR(avi);
	} else if (flags & XATTR_REPLACE) {
		err = -ENOATTR;
	} else {
		/*
		 * Create the stream empty (a resident record that always fits
		 * the base mft record) and fill it through the attribute inode,
		 * which turns it non-resident as needed. Passing the value to
		 * ntfs_attr_add() would size a resident record for it first
		 * and, for anything larger than the mft record, grow an
		 * attribute list and extent record for nothing.
		 */
		mutex_lock(&ni->mrec_lock);
		err = ntfs_attr_add(ni, AT_DATA, uname, ulen, NULL, 0);
		mutex_unlock(&ni->mrec_lock);
		if (!err && size) {
			avi = ntfs_attr_iget(vi, AT_DATA, uname, ulen);
			if (IS_ERR(avi)) {
				err = PTR_ERR(avi);
			} else {
				s64 n;

				inode_lock(avi);
				n = do_write(avi, buf, size, 0);
				inode_unlock(avi);
				if (n < 0)
					err = (int)n;
				else if (n != (s64)size)
					err = -EIO;
				iput(avi);
			}
		}
	}
	if (!err) {
		inode_set_ctime_current(vi);
		mark_inode_dirty(vi);
	}
	inode_unlock(vi);
free:
	kmem_cache_free(ntfs_name_cache, uname);
	return err;
}

int ntfs_removexattr(ntfs_inode_t *h, const char *name)
{
	struct inode *vi = VI(h), *avi;
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	__le16 *uname;
	int ulen, err;

	if (!h)
		return -EINVAL;
	if (IS_RDONLY(vi))
		return -EROFS;
	if (NVolShutdown(vol))
		return -EIO;
	ulen = xattr_uname(vol, name, &uname);
	if (ulen < 0)
		return ulen;

	inode_lock(vi);
	if (!(vol->vol_flags & VOLUME_IS_DIRTY))
		ntfs_set_volume_flags(vol, VOLUME_IS_DIRTY);
	avi = ntfs_attr_iget(vi, AT_DATA, uname, ulen);
	if (IS_ERR(avi)) {
		err = PTR_ERR(avi) == -ENOENT ? -ENOATTR : PTR_ERR(avi);
	} else {
		truncate_inode_pages(avi->i_mapping, 0);
		mutex_lock(&ni->mrec_lock);
		err = ntfs_attr_rm(NTFS_I(avi));
		mutex_unlock(&ni->mrec_lock);
		/* ntfs_attr_rm() returns ntfs_cluster_free()'s positive count for
		 * a non-resident stream; only a negative value is an error. */
		if (err > 0)
			err = 0;
		/* The attribute inode must not be found again. */
		remove_inode_hash(avi);
		iput(avi);
		if (!err) {
			inode_set_ctime_current(vi);
			mark_inode_dirty(vi);
		}
	}
	inode_unlock(vi);
	kmem_cache_free(ntfs_name_cache, uname);
	return err;
}

int ntfs_listxattr(ntfs_inode_t *h, char *buf, size_t size, size_t *len_out)
{
	struct inode *vi = VI(h);
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_attr_search_ctx *ctx;
	size_t total = 0;
	int err = 0;

	if (!h || !len_out)
		return -EINVAL;
	*len_out = 0;
	inode_lock_shared(vi);
	mutex_lock(&ni->mrec_lock);
	ctx = ntfs_attr_get_search_ctx(ni, NULL);
	if (!ctx) {
		err = -ENOMEM;
		goto out;
	}
	while (!(err = ntfs_attrs_walk(ctx))) {
		struct attr_record *a = ctx->attr;
		unsigned char *s = NULL;
		int slen;

		if (a->type != AT_DATA || !a->name_length)
			continue;
		slen = ntfs_ucstonls(ni->vol, (__le16 *)((u8 *)a + le16_to_cpu(a->name_offset)),
				     a->name_length, &s, 0);
		if (slen < 0) {
			err = slen;
			break;
		}
		if (buf) {
			if (total + slen + 1 > size) {
				kfree(s);
				err = -ERANGE;
				break;
			}
			memcpy(buf + total, s, slen);
			buf[total + slen] = 0;
		}
		total += slen + 1;
		kfree(s);
	}
	if (err == -ENOENT)
		err = 0;
	ntfs_attr_put_search_ctx(ctx);
out:
	mutex_unlock(&ni->mrec_lock);
	inode_unlock_shared(vi);
	if (!err)
		*len_out = total;
	return err;
}

/* ---- maintenance ------------------------------------------------------ */

int ntfs_validate_name(ntfs_volume_t *h, const char *name)
{
	__le16 *uname;
	size_t len;
	int ulen, err;

	if (!h)
		return -EINVAL;
	err = check_component(name, &len);
	if (err)
		return err;
	ulen = ntfs_nlstoucs(h->vol, name, (int)len, &uname, NTFS_MAX_NAME_LEN);
	if (ulen < 0)
		return ulen == -EILSEQ ? -EINVAL : ulen;
	err = ntfs_vfs_validate_uname(h->vol, uname, ulen);
	kmem_cache_free(ntfs_name_cache, uname);
	return err;
}
