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

// Flags for ntfs_setxattr(). The core is built against platform/include/
// linux/xattr.h, so it expects the LINUX values (1 / 2), not Darwin's
// <sys/xattr.h> XATTR_CREATE=2 / XATTR_REPLACE=4. Never pass the Darwin ones.
#define NTFS_XATTR_CREATE_FLAG  0x1
#define NTFS_XATTR_REPLACE_FLAG 0x2

// NTFS FILE_ATTR_* bits we translate to/from BSD st_flags (ntfs_attr.file_attributes).
#define NTFS_FILE_ATTR_READONLY 0x00000001u
#define NTFS_FILE_ATTR_HIDDEN   0x00000002u
#define NTFS_FILE_ATTR_SYSTEM   0x00000004u
#define NTFS_FILE_ATTR_ARCHIVE  0x00000020u
