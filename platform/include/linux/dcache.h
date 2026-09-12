/* SPDX-License-Identifier: GPL-2.0 */
/* Dentries do not exist in the port. The struct is kept minimal so that
 * signatures in Tier 1 (statfs, xattr handlers) still type-check; the vfs
 * stream never creates real dentries. */
#ifndef _LINUX_DCACHE_H
#define _LINUX_DCACHE_H
#include <linux/types.h>
#include <linux/fs.h>
struct qstr { const unsigned char *name; u32 len; u32 hash; };
struct dentry { struct inode *d_inode; struct super_block *d_sb; struct qstr d_name; struct dentry *d_parent; void *d_fsdata; };
static inline struct inode *d_inode(const struct dentry *d) { return d->d_inode; }
static inline bool d_is_dir(const struct dentry *d) { return d->d_inode && S_ISDIR(d->d_inode->i_mode); }
struct dentry *d_make_root(struct inode *root);
static inline void dput(struct dentry *d) { (void)d; }
static inline struct dentry *dget(struct dentry *d) { return d; }
#endif
