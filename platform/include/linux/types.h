/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel scalar types for the user-space port. Contract header: every
 * component includes this, directly or through other linux/ shims.
 */
#ifndef _LINUX_TYPES_H
#define _LINUX_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <ntfsport/config.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef u8  __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef s8  __s8;
typedef s16 __s16;
typedef s32 __s32;
typedef s64 __s64;

/* Little/big-endian annotated integers. Plain integers here; the driver
 * always converts through the cpu_to_leN / leN_to_cpu helpers. */
typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;
typedef __le16 le16;
typedef __le32 le32;
typedef __le64 le64;
typedef __le16 sle16;
typedef __le32 sle32;
typedef __le64 sle64;

typedef s64 loff_t;
typedef u64 sector_t;
/* blkcnt_t comes from <sys/types.h> (signed on Darwin; fine). */
typedef u64 pgoff_t;
typedef unsigned short umode_t;
typedef unsigned int gfp_t;
typedef unsigned int fmode_t;
typedef u32 errseq_t;
typedef s64 ktime_t;

typedef struct { uid_t val; } kuid_t;
typedef struct { gid_t val; } kgid_t;

typedef struct { volatile int counter; } atomic_t;
typedef struct { volatile s64 counter; } atomic64_t;
typedef atomic64_t atomic_long_t;

struct list_head {
	struct list_head *next, *prev;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

struct rb_node {
	unsigned long __rb_parent_color;
	struct rb_node *rb_right;
	struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rb_root {
	struct rb_node *rb_node;
};
#define RB_ROOT (struct rb_root) { NULL, }

struct timespec64 {
	s64 tv_sec;
	long tv_nsec;
};

#define BITS_PER_LONG 64
#define BITS_PER_BYTE 8

#define PAGE_SHIFT NTFS_PAGE_SHIFT
#define PAGE_SIZE  (1UL << PAGE_SHIFT)
#define PAGE_MASK  (~(PAGE_SIZE - 1))

#endif /* _LINUX_TYPES_H */
