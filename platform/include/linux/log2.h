/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_LOG2_H
#define _LINUX_LOG2_H

#include <linux/types.h>

static inline bool is_power_of_2(unsigned long n) { return n != 0 && (n & (n - 1)) == 0; }
static inline int ilog2_u64(u64 n) { return n ? 63 - __builtin_clzll(n) : -1; }
#define ilog2(n) ilog2_u64((u64)(n))
static inline unsigned long roundup_pow_of_two(unsigned long n) { return n <= 1 ? 1 : 1UL << (ilog2(n - 1) + 1); }
static inline unsigned long rounddown_pow_of_two(unsigned long n) { return 1UL << ilog2(n); }
static inline int order_base_2(unsigned long n) { return n > 1 ? ilog2(n - 1) + 1 : 0; }
#define get_order(n) order_base_2(((n) + PAGE_SIZE - 1) >> PAGE_SHIFT)

#endif /* _LINUX_LOG2_H */
