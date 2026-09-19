/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_UIDGID_H
#define _LINUX_UIDGID_H

#include <unistd.h>
#include <linux/types.h>

struct user_namespace;
struct mnt_idmap;
#define nop_mnt_idmap ((struct mnt_idmap *)0)
#define init_user_ns ((struct user_namespace *)0)
#define current_user_ns() init_user_ns

#define KUIDT_INIT(v) (kuid_t){ v }
#define KGIDT_INIT(v) (kgid_t){ v }
#define GLOBAL_ROOT_UID KUIDT_INIT(0)
#define GLOBAL_ROOT_GID KGIDT_INIT(0)
#define INVALID_UID KUIDT_INIT((uid_t)-1)
#define INVALID_GID KGIDT_INIT((gid_t)-1)

static inline uid_t __kuid_val(kuid_t u) { return u.val; }
static inline gid_t __kgid_val(kgid_t g) { return g.val; }
static inline bool uid_eq(kuid_t a, kuid_t b) { return a.val == b.val; }
static inline bool gid_eq(kgid_t a, kgid_t b) { return a.val == b.val; }
static inline bool uid_valid(kuid_t u) { return u.val != (uid_t)-1; }
static inline bool gid_valid(kgid_t g) { return g.val != (gid_t)-1; }
static inline kuid_t make_kuid(struct user_namespace *ns, uid_t uid) { (void)ns; return KUIDT_INIT(uid); }
static inline kgid_t make_kgid(struct user_namespace *ns, gid_t gid) { (void)ns; return KGIDT_INIT(gid); }
static inline uid_t from_kuid(struct user_namespace *ns, kuid_t u) { (void)ns; return u.val; }
static inline gid_t from_kgid(struct user_namespace *ns, kgid_t g) { (void)ns; return g.val; }
static inline uid_t from_kuid_munged(struct user_namespace *ns, kuid_t u) { (void)ns; return u.val; }
static inline gid_t from_kgid_munged(struct user_namespace *ns, kgid_t g) { (void)ns; return g.val; }
static inline kuid_t current_fsuid(void) { return KUIDT_INIT(geteuid()); }
static inline kgid_t current_fsgid(void) { return KGIDT_INIT(getegid()); }
static inline kuid_t current_uid(void) { return KUIDT_INIT(getuid()); }

/* idmapped-mount helpers used by the core reduce to identity. */
#define vfsuid_into_kuid(x) (x)
#define vfsgid_into_kgid(x) (x)

#endif /* _LINUX_UIDGID_H */
