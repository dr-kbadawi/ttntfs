/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_SPINLOCK_H
#define _LINUX_SPINLOCK_H

#include <pthread.h>
#include <linux/types.h>

/* User space has no IRQs; spinlocks and rwlocks are plain pthread locks and
 * the *_irqsave variants just ignore the flags argument. */
typedef struct { pthread_mutex_t m; } spinlock_t;
typedef struct { pthread_rwlock_t rw; } rwlock_t;

#define DEFINE_SPINLOCK(x) spinlock_t x = { PTHREAD_MUTEX_INITIALIZER }
#define __SPIN_LOCK_UNLOCKED(x) { PTHREAD_MUTEX_INITIALIZER }
#define DEFINE_RWLOCK(x) rwlock_t x = { PTHREAD_RWLOCK_INITIALIZER }

static inline void spin_lock_init(spinlock_t *l) { pthread_mutex_init(&l->m, NULL); }
static inline void spin_lock(spinlock_t *l) { pthread_mutex_lock(&l->m); }
static inline void spin_unlock(spinlock_t *l) { pthread_mutex_unlock(&l->m); }
static inline int spin_trylock(spinlock_t *l) { return pthread_mutex_trylock(&l->m) == 0; }
#define spin_lock_irqsave(l, flags) do { (void)(flags); spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, flags) do { (void)(flags); spin_unlock(l); } while (0)
#define spin_lock_irq(l) spin_lock(l)
#define spin_unlock_irq(l) spin_unlock(l)
#define spin_lock_bh(l) spin_lock(l)
#define spin_unlock_bh(l) spin_unlock(l)

static inline void rwlock_init(rwlock_t *l) { pthread_rwlock_init(&l->rw, NULL); }
static inline void read_lock(rwlock_t *l) { pthread_rwlock_rdlock(&l->rw); }
static inline void read_unlock(rwlock_t *l) { pthread_rwlock_unlock(&l->rw); }
static inline void write_lock(rwlock_t *l) { pthread_rwlock_wrlock(&l->rw); }
static inline void write_unlock(rwlock_t *l) { pthread_rwlock_unlock(&l->rw); }
#define read_lock_irqsave(l, flags) do { (void)(flags); read_lock(l); } while (0)
#define read_unlock_irqrestore(l, flags) do { (void)(flags); read_unlock(l); } while (0)
#define write_lock_irqsave(l, flags) do { (void)(flags); write_lock(l); } while (0)
#define write_unlock_irqrestore(l, flags) do { (void)(flags); write_unlock(l); } while (0)

#endif /* _LINUX_SPINLOCK_H */
