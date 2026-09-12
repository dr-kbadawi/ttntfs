/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STAT_H
#define _LINUX_STAT_H

#include <sys/stat.h>
#include <linux/types.h>
#include <linux/time64.h>
#include <linux/uidgid.h>

/* S_IF*, S_IS*, S_IRWXU... come from <sys/stat.h>; identical on Darwin. */
#define S_IRWXUGO (S_IRWXU | S_IRWXG | S_IRWXO)
#define S_IALLUGO (S_ISUID | S_ISGID | S_ISVTX | S_IRWXUGO)
#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO (S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)

/* struct kstat used by getattr paths; kept for the vfs layer. */
#define STATX_TYPE	0x00000001U
#define STATX_MODE	0x00000002U
#define STATX_NLINK	0x00000004U
#define STATX_UID	0x00000008U
#define STATX_GID	0x00000010U
#define STATX_ATIME	0x00000020U
#define STATX_MTIME	0x00000040U
#define STATX_CTIME	0x00000080U
#define STATX_INO	0x00000100U
#define STATX_SIZE	0x00000200U
#define STATX_BLOCKS	0x00000400U
#define STATX_BASIC_STATS 0x000007ffU
#define STATX_BTIME	0x00000800U
#define STATX_ATTR_COMPRESSED	0x00000004
#define STATX_ATTR_IMMUTABLE	0x00000010
#define STATX_ATTR_APPEND	0x00000020
#define STATX_ATTR_NODUMP	0x00000040
#define STATX_ATTR_ENCRYPTED	0x00000800

struct kstat {
	u32 result_mask;
	umode_t mode;
	unsigned int nlink;
	u32 blksize;
	u64 attributes;
	u64 attributes_mask;
	u64 ino;
	dev_t dev;
	dev_t rdev;
	kuid_t uid;
	kgid_t gid;
	loff_t size;
	struct timespec64 atime, mtime, ctime, btime;
	u64 blocks;
};

/* iattr for setattr paths. */
#define ATTR_MODE	(1 << 0)
#define ATTR_UID	(1 << 1)
#define ATTR_GID	(1 << 2)
#define ATTR_SIZE	(1 << 3)
#define ATTR_ATIME	(1 << 4)
#define ATTR_MTIME	(1 << 5)
#define ATTR_CTIME	(1 << 6)
#define ATTR_ATIME_SET	(1 << 7)
#define ATTR_MTIME_SET	(1 << 8)
#define ATTR_FORCE	(1 << 9)
#define ATTR_KILL_SUID	(1 << 11)
#define ATTR_KILL_SGID	(1 << 12)
#define ATTR_FILE	(1 << 13)
#define ATTR_KILL_PRIV	(1 << 14)
#define ATTR_OPEN	(1 << 15)
#define ATTR_TIMES_SET	(1 << 16)
#define ATTR_TOUCH	(1 << 17)

struct iattr {
	unsigned int ia_valid;
	umode_t ia_mode;
	kuid_t ia_uid;
	kgid_t ia_gid;
	loff_t ia_size;
	struct timespec64 ia_atime, ia_mtime, ia_ctime;
};

#endif /* _LINUX_STAT_H */
