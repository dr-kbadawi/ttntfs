/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_MUTEX_H
#define _LINUX_MUTEX_H

#include <pthread.h>
#include <linux/types.h>

struct mutex {
	pthread_mutex_t m;
};

#define DEFINE_MUTEX(name) struct mutex name = { PTHREAD_MUTEX_INITIALIZER }
#define __MUTEX_INITIALIZER(name) { PTHREAD_MUTEX_INITIALIZER }

static inline void mutex_init(struct mutex *l) { pthread_mutex_init(&l->m, NULL); }
static inline void mutex_destroy(struct mutex *l) { pthread_mutex_destroy(&l->m); }
static inline void mutex_lock(struct mutex *l) { pthread_mutex_lock(&l->m); }
static inline void mutex_unlock(struct mutex *l) { pthread_mutex_unlock(&l->m); }
static inline int mutex_trylock(struct mutex *l) { return pthread_mutex_trylock(&l->m) == 0; }
static inline int mutex_lock_interruptible(struct mutex *l) { mutex_lock(l); return 0; }
static inline bool mutex_is_locked(struct mutex *l)
{
	if (pthread_mutex_trylock(&l->m) == 0) { pthread_mutex_unlock(&l->m); return false; }
	return true;
}
/* Lockdep subclasses have no meaning here. */
#define mutex_lock_nested(l, subclass) mutex_lock(l)
#define mutex_init_with_class(l, cls) mutex_init(l)

#endif /* _LINUX_MUTEX_H */
