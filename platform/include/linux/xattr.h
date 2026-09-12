/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_XATTR_H
#define _LINUX_XATTR_H
#include <linux/types.h>
#include <linux/fs.h>
#define XATTR_CREATE 0x1
#define XATTR_REPLACE 0x2
#define XATTR_NAME_MAX 255
#define XATTR_SIZE_MAX 65536
#define XATTR_LIST_MAX 65536
#define XATTR_USER_PREFIX "user."
#define XATTR_USER_PREFIX_LEN 5
#define XATTR_SYSTEM_PREFIX "system."
#define XATTR_SYSTEM_PREFIX_LEN 7
#define XATTR_TRUSTED_PREFIX "trusted."
#define XATTR_SECURITY_PREFIX "security."
#define XATTR_NAME_POSIX_ACL_ACCESS "system.posix_acl_access"
#define XATTR_NAME_POSIX_ACL_DEFAULT "system.posix_acl_default"
struct xattr_handler {
	const char *name;
	const char *prefix;
	int flags;
	bool (*list)(struct dentry *dentry);
	int (*get)(const struct xattr_handler *, struct dentry *dentry, struct inode *inode, const char *name, void *buffer, size_t size);
	int (*set)(const struct xattr_handler *, struct mnt_idmap *idmap, struct dentry *dentry, struct inode *inode, const char *name, const void *buffer, size_t size, int flags);
};
static inline const char *xattr_prefix(const struct xattr_handler *h) { return h->prefix ?: h->name; }
static inline bool xattr_can_list(const struct xattr_handler *h, struct dentry *d) { (void)d; return !h->list || h->list(d); }
#endif
