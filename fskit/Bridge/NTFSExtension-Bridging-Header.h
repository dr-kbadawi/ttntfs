// Bridging header for the FSKit extension target.
//
// ntfsport/bdev.h is intentionally NOT imported here (see ntfs_bdev_fskit.h):
// its kernel-compat types collide with Darwin's <sys/rbtree.h> under modules.
#include <errno.h>
#include <sys/types.h>
#include "ntfscore.h"
#import "ntfs_bdev_fskit.h"

// Maps a Linux-only errno the core may return (EUCLEAN, ENOMEDIUM, ...) onto
// the Darwin errno FSKit expects. static inline in platform/include/linux/errno.h;
// Swift calls it directly. Every negative return from the core goes through it
// (see Errors.swift, hostErrno()).
#include <linux/errno.h>

// Flags for ntfs_setxattr() are NOT defined here any more. NTFS_XATTR_CREATE
// and NTFS_XATTR_REPLACE come in with ntfscore.h above (core/include/
// ntfs_xattr_flags.h), so Swift sees the same two numbers the core was compiled
// with instead of a copy of them that nothing checked. This file used to
// restate them as NTFS_XATTR_CREATE_FLAG / NTFS_XATTR_REPLACE_FLAG; it happened
// to have them right, but nothing would have caught it if it had not.
// Never pass Darwin's <sys/xattr.h> XATTR_CREATE / XATTR_REPLACE: same names,
// different numbers (2 and 4), and the mismatch is silent.

// NTFS FILE_ATTR_* bits we translate to/from BSD st_flags (ntfs_attr.file_attributes).
#define NTFS_FILE_ATTR_READONLY 0x00000001u
#define NTFS_FILE_ATTR_HIDDEN   0x00000002u
#define NTFS_FILE_ATTR_SYSTEM   0x00000004u
#define NTFS_FILE_ATTR_ARCHIVE  0x00000020u
