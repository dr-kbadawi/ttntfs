/* SPDX-License-Identifier: GPL-2.0 */
/* POSIX ACLs are not mapped in v1 (docs/PORTING.md §6). The types exist so
 * ea.c compiles; every operation reports "no ACL". */
#ifndef _LINUX_POSIX_ACL_H
#define _LINUX_POSIX_ACL_H
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/err.h>
#define ACL_TYPE_ACCESS 0x8000
#define ACL_TYPE_DEFAULT 0x4000
#define ACL_USER_OBJ 0x01
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20
struct posix_acl_entry { short e_tag; unsigned short e_perm; union { kuid_t e_uid; kgid_t e_gid; }; };
struct posix_acl { atomic_t a_refcount; unsigned int a_count; struct posix_acl_entry a_entries[]; };
#define ACL_NOT_CACHED ((void *)(-1))
static inline void posix_acl_release(struct posix_acl *acl) { (void)acl; }
static inline struct posix_acl *posix_acl_from_xattr(struct user_namespace *ns, const void *v, size_t s) { (void)ns; (void)v; (void)s; return ERR_PTR(-EOPNOTSUPP); }
static inline int posix_acl_to_xattr(struct user_namespace *ns, const struct posix_acl *acl, void *b, size_t s) { (void)ns; (void)acl; (void)b; (void)s; return -EOPNOTSUPP; }
static inline int posix_acl_create(struct inode *dir, umode_t *mode, struct posix_acl **def, struct posix_acl **acc)
{ (void)dir; (void)mode; *def = NULL; *acc = NULL; return 0; }
static inline int posix_acl_update_mode(struct mnt_idmap *m, struct inode *i, umode_t *mode, struct posix_acl **acl) { (void)m; (void)i; (void)mode; (void)acl; return 0; }
static inline void set_cached_acl(struct inode *i, int type, struct posix_acl *acl) { (void)i; (void)type; (void)acl; }
static inline struct posix_acl *get_cached_acl(struct inode *i, int type) { (void)i; (void)type; return NULL; }
static inline void forget_all_cached_acls(struct inode *i) { (void)i; }
static inline void cache_no_acl(struct inode *i) { (void)i; }
#endif
