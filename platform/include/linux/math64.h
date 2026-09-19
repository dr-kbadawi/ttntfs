/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_MATH64_H
#define _LINUX_MATH64_H

#include <linux/types.h>
#include <asm/div64.h>

static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{ *remainder = dividend % divisor; return dividend / divisor; }
static inline s64 div_s64_rem(s64 dividend, s32 divisor, s32 *remainder)
{ *remainder = dividend % divisor; return dividend / divisor; }
static inline u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{ *remainder = dividend % divisor; return dividend / divisor; }
static inline u64 div_u64(u64 dividend, u32 divisor) { return dividend / divisor; }
static inline s64 div_s64(s64 dividend, s32 divisor) { return dividend / divisor; }
static inline u64 div64_u64(u64 dividend, u64 divisor) { return dividend / divisor; }
static inline s64 div64_s64(s64 dividend, s64 divisor) { return dividend / divisor; }
static inline u64 mul_u64_u32_shr(u64 a, u32 mul, unsigned int shift) { return (u64)(((unsigned __int128)a * mul) >> shift); }
#define DIV64_U64_ROUND_UP(ll, d) div64_u64((ll) + (d) - 1, (d))

#endif /* _LINUX_MATH64_H */
