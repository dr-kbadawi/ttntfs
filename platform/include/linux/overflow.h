/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_OVERFLOW_H
#define _LINUX_OVERFLOW_H
#include <linux/types.h>
#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define check_sub_overflow(a, b, d) __builtin_sub_overflow(a, b, d)
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
/* T may be a type name or an expression (kernel semantics). */
#define overflows_type(n, T) ({ __typeof__(n) __n = (n); __typeof__(T) __t = (__typeof__(T))__n; (__typeof__(n))__t != __n || ((__n < 0) != (__t < 0)); })
#define struct_size(p, member, count) (sizeof(*(p)) + sizeof(*(p)->member) * (count))
#define flex_array_size(p, member, count) (sizeof(*(p)->member) * (count))
#endif
