/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_ERR_H
#define _LINUX_ERR_H

#include <linux/types.h>
#include <linux/compiler.h>

#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)

static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline bool IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
static inline bool IS_ERR_OR_NULL(const void *ptr) { return unlikely(!ptr) || IS_ERR_VALUE((unsigned long)ptr); }
static inline void *ERR_CAST(const void *ptr) { return (void *)ptr; }
static inline int PTR_ERR_OR_ZERO(const void *ptr) { return IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0; }

#endif /* _LINUX_ERR_H */
