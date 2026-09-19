/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * vfs.h - private glue shared by the Tier 2 replacement in core/vfs/.
 *
 * PORT: everything here stands in for Linux VFS helpers the upstream
 * inode.c/namei.c/file.c used and that the compat layer does not provide,
 * plus the entry points api.c calls instead of inode_operations tables.
 */
#ifndef NTFS_VFS_H
#define NTFS_VFS_H

#include <linux/fs.h>
#include <linux/writeback.h>
#include <linux/falloc.h>
#include <linux/stat.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/fs_context.h>
#include "ntfs.h"

/* ---- small compat helpers ------------------------------------------- */

#define alloc_inode_sb(sb, cache, gfp) kmem_cache_alloc((cache), (gfp))
#define kvrealloc(p, size, gfp) krealloc((p), (size), (gfp))
#ifndef FGP_WRITEBEGIN
#define FGP_WRITEBEGIN (FGP_LOCK | FGP_CREAT | FGP_STABLE)
#endif
#ifndef FALLOC_FL_ALLOCATE_RANGE
#define FALLOC_FL_ALLOCATE_RANGE 0
#endif
#ifndef LLONG_MAX
#define LLONG_MAX ((long long)(~0ULL >> 1))
#endif

static inline void init_special_inode(struct inode *inode, umode_t mode, dev_t rdev)
{
	inode->i_mode = mode;
	inode->i_rdev = rdev;
}

/* Kernel's simple_inode_init_ts(): stamp all three times with "now". */
static inline struct timespec64 simple_inode_init_ts(struct inode *inode)
{
	struct timespec64 ts = inode_set_ctime_current(inode);

	inode_set_atime_to_ts(inode, ts);
	inode_set_mtime_to_ts(inode, ts);
	return ts;
}

static inline int inode_newsize_ok(const struct inode *inode, loff_t newsize)
{
	if (newsize < 0)
		return -EINVAL;
	if (newsize > inode->i_sb->s_maxbytes)
		return -EFBIG;
	return 0;
}

/*
 * Mark a freshly allocated inode I_NEW so that concurrent iget()s block on
 * it until unlock_new_inode(). The platform inode table blocks waiters on
 * i_new_lock, so take it here; unlock_new_inode() releases it.
 */
static inline void ntfs_vfs_mark_new(struct inode *vi)
{
	mutex_lock(&vi->i_new_lock);
	spin_lock(&vi->i_lock);
	vi->i_state |= I_NEW | I_CREATING;
	spin_unlock(&vi->i_lock);
}

/* ---- aops.c ----------------------------------------------------------- */

/*
 * Zero [start, start + len) of a non-resident attribute on the device:
 * allocated clusters are zeroed (through the page cache for partial pages,
 * directly for whole pages), holes are skipped. Replaces iomap_zero_range().
 */
int ntfs_vfs_zero_range(struct inode *vi, loff_t start, loff_t len);
/* Zero a byte range of the device directly (cluster/sector aligned). */
int ntfs_dio_zero_range(struct inode *vi, loff_t offset, loff_t length);
/* Read/write [pos, pos+len) of a non-resident attribute straight from the
 * run list to the device. pos/len are sector aligned. Holes read as zero;
 * a hole under a write is an error. */
int ntfs_vfs_direct_read(struct inode *vi, void *buf, loff_t pos, size_t len);
int ntfs_vfs_direct_write(struct inode *vi, const void *buf, loff_t pos, size_t len);

/* ---- namei.c ---------------------------------------------------------- */

int ntfs_check_bad_windows_name(struct ntfs_volume *vol, const __le16 *wc,
				unsigned int wc_len);
/* Name policy for the mount (docs/PORTING.md §6). */
int ntfs_vfs_validate_uname(struct ntfs_volume *vol, const __le16 *uname, int len);
struct inode *ntfs_vfs_lookup(struct inode *dir, const char *name, int len);
struct inode *ntfs_vfs_create(struct inode *dir, const char *name, int len,
			      umode_t mode, const char *target);
int ntfs_vfs_unlink(struct inode *dir, const char *name, int len, bool rmdir);
int ntfs_vfs_link(struct inode *vi, struct inode *dir, const char *name, int len);
int ntfs_vfs_rename(struct inode *old_dir, const char *old_name, int old_len,
		    struct inode *new_dir, const char *new_name, int new_len);
/* Parent directory of @vi from its first $FILE_NAME (for ".."). */
u64 ntfs_vfs_parent_ino(struct inode *vi);

/* ---- file.c ----------------------------------------------------------- */

int ntfs_vfs_fsync(struct inode *vi, bool datasync);
int ntfs_vfs_setattr(struct inode *vi, struct iattr *attr);
int ntfs_vfs_fallocate(struct inode *vi, int mode, loff_t offset, loff_t len);
int ntfs_vfs_trim_prealloc(struct inode *vi);

#endif /* NTFS_VFS_H */
