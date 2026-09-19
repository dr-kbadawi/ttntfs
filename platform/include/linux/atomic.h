/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_ATOMIC_H
#define _LINUX_ATOMIC_H

#include <linux/types.h>

#define ATOMIC_INIT(i) { (i) }
#define ATOMIC64_INIT(i) { (i) }

static inline int atomic_read(const atomic_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST); }
static inline void atomic_set(atomic_t *v, int i) { __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_inc(atomic_t *v) { __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline void atomic_dec(atomic_t *v) { __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline int atomic_inc_return(atomic_t *v) { return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline int atomic_dec_return(atomic_t *v) { return __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline void atomic_add(int i, atomic_t *v) { __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_sub(int i, atomic_t *v) { __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_add_return(int i, atomic_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline bool atomic_dec_and_test(atomic_t *v) { return atomic_dec_return(v) == 0; }
static inline bool atomic_inc_not_zero(atomic_t *v)
{
	int c = atomic_read(v);
	while (c != 0) {
		if (__atomic_compare_exchange_n(&v->counter, &c, c + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return true;
	}
	return false;
}
static inline int atomic_cmpxchg(atomic_t *v, int old, int new_)
{
	__atomic_compare_exchange_n(&v->counter, &old, new_, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return old;
}

static inline s64 atomic64_read(const atomic64_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST); }
static inline void atomic64_set(atomic64_t *v, s64 i) { __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_add(s64 i, atomic64_t *v) { __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_sub(s64 i, atomic64_t *v) { __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_inc(atomic64_t *v) { atomic64_add(1, v); }
static inline void atomic64_dec(atomic64_t *v) { atomic64_sub(1, v); }
static inline s64 atomic64_add_return(s64 i, atomic64_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_sub_return(s64 i, atomic64_t *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_inc_return(atomic64_t *v) { return atomic64_add_return(1, v); }
static inline s64 atomic64_dec_return(atomic64_t *v) { return atomic64_sub_return(1, v); }

#define atomic_long_read atomic64_read
#define atomic_long_set atomic64_set
#define atomic_long_inc atomic64_inc
#define atomic_long_dec atomic64_dec

#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_mb__before_atomic() smp_mb()
#define smp_mb__after_atomic() smp_mb()
#define barrier() __asm__ __volatile__("" : : : "memory")

#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, val) __atomic_store_n(&(x), (val), __ATOMIC_RELAXED)

#endif /* _LINUX_ATOMIC_H */
