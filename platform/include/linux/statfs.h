/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STATFS_H
#define _LINUX_STATFS_H
#include <linux/types.h>
struct kstatfs { long f_type; long f_bsize; u64 f_blocks; u64 f_bfree; u64 f_bavail; u64 f_files; u64 f_ffree; struct { int val[2]; } f_fsid; long f_namelen; long f_frsize; long f_flags; long f_spare[4]; };
static inline __attribute__((unused)) __typeof__(((struct kstatfs *)0)->f_fsid) u64_to_fsid(u64 v) { __typeof__(((struct kstatfs *)0)->f_fsid) f; f.val[0] = (int)(v & 0xffffffff); f.val[1] = (int)(v >> 32); return f; }
#endif
