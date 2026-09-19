/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _ASM_DIV64_H
#define _ASM_DIV64_H

#include <linux/types.h>

/* do_div(n, base): n /= base, returns remainder. n is an lvalue of u64. */
#define do_div(n, base) ({ \
	u32 __base = (base); \
	u32 __rem = (u32)((n) % __base); \
	(n) = (n) / __base; \
	__rem; })

#endif /* _ASM_DIV64_H */
