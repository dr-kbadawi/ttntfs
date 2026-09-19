/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_POSIX_ACL_XATTR_H
#define _LINUX_POSIX_ACL_XATTR_H
#include <linux/posix_acl.h>
#include <linux/xattr.h>
static inline size_t posix_acl_xattr_size(int count) { return 4 + 8 * count; }
#endif
