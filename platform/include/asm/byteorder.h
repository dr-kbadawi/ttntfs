/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_BYTEORDER_H
#define _ASM_BYTEORDER_H

#include <linux/types.h>

/* Darwin on arm64 and x86_64 is little-endian. */
#if !defined(__LITTLE_ENDIAN__) && !(defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#error "big-endian hosts are not supported"
#endif
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN

#define cpu_to_le16(x) ((__le16)(u16)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define cpu_to_le64(x) ((__le64)(u64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define const_cpu_to_le16(x) cpu_to_le16(x)
#define const_cpu_to_le32(x) cpu_to_le32(x)
#define const_cpu_to_le64(x) cpu_to_le64(x)
#define const_le16_to_cpu(x) le16_to_cpu(x)
#define const_le32_to_cpu(x) le32_to_cpu(x)
#define const_le64_to_cpu(x) le64_to_cpu(x)
#define __cpu_to_le16 cpu_to_le16
#define __cpu_to_le32 cpu_to_le32
#define __cpu_to_le64 cpu_to_le64
#define __le16_to_cpu le16_to_cpu
#define __le32_to_cpu le32_to_cpu
#define __le64_to_cpu le64_to_cpu

#define cpu_to_be16(x) ((__be16)__builtin_bswap16((u16)(x)))
#define cpu_to_be32(x) ((__be32)__builtin_bswap32((u32)(x)))
#define cpu_to_be64(x) ((__be64)__builtin_bswap64((u64)(x)))
#define be16_to_cpu(x) __builtin_bswap16((u16)(x))
#define be32_to_cpu(x) __builtin_bswap32((u32)(x))
#define be64_to_cpu(x) __builtin_bswap64((u64)(x))

static inline u16 le16_to_cpup(const __le16 *p) { u16 v; __builtin_memcpy(&v, p, 2); return v; }
static inline u32 le32_to_cpup(const __le32 *p) { u32 v; __builtin_memcpy(&v, p, 4); return v; }
static inline u64 le64_to_cpup(const __le64 *p) { u64 v; __builtin_memcpy(&v, p, 8); return v; }
static inline void le16_add_cpu(__le16 *var, u16 val) { *var = cpu_to_le16(le16_to_cpu(*var) + val); }
static inline void le32_add_cpu(__le32 *var, u32 val) { *var = cpu_to_le32(le32_to_cpu(*var) + val); }
static inline void le64_add_cpu(__le64 *var, u64 val) { *var = cpu_to_le64(le64_to_cpu(*var) + val); }

/* Unaligned accessors. */
static inline u16 get_unaligned_le16(const void *p) { u16 v; __builtin_memcpy(&v, p, 2); return v; }
static inline u32 get_unaligned_le32(const void *p) { u32 v; __builtin_memcpy(&v, p, 4); return v; }
static inline u64 get_unaligned_le64(const void *p) { u64 v; __builtin_memcpy(&v, p, 8); return v; }
static inline void put_unaligned_le16(u16 v, void *p) { __builtin_memcpy(p, &v, 2); }
static inline void put_unaligned_le32(u32 v, void *p) { __builtin_memcpy(p, &v, 4); }
static inline void put_unaligned_le64(u64 v, void *p) { __builtin_memcpy(p, &v, 8); }

#endif /* _ASM_BYTEORDER_H */
