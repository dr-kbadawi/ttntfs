/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RWSEM_H
#define _LINUX_RWSEM_H

#include <pthread.h>
#include <linux/types.h>

struct rw_semaphore {
	pthread_rwlock_t rw;
};

#define DECLARE_RWSEM(name) struct rw_semaphore name = { PTHREAD_RWLOCK_INITIALIZER }
#define __RWSEM_INITIALIZER(name) { PTHREAD_RWLOCK_INITIALIZER }

static inline void init_rwsem(struct rw_semaphore *s) { pthread_rwlock_init(&s->rw, NULL); }
static inline void destroy_rwsem(struct rw_semaphore *s) { pthread_rwlock_destroy(&s->rw); }
static inline void down_read(struct rw_semaphore *s) { pthread_rwlock_rdlock(&s->rw); }
static inline void down_write(struct rw_semaphore *s) { pthread_rwlock_wrlock(&s->rw); }
static inline int down_read_trylock(struct rw_semaphore *s) { return pthread_rwlock_tryrdlock(&s->rw) == 0; }
static inline int down_write_trylock(struct rw_semaphore *s) { return pthread_rwlock_trywrlock(&s->rw) == 0; }
static inline void up_read(struct rw_semaphore *s) { pthread_rwlock_unlock(&s->rw); }
static inline void up_write(struct rw_semaphore *s) { pthread_rwlock_unlock(&s->rw); }
/* pthread has no atomic downgrade; release write and reacquire read. The
 * driver uses downgrade only where a brief gap is tolerable (runlist
 * remapping); revisit if a caller depends on atomicity. */
static inline void downgrade_write(struct rw_semaphore *s) { up_write(s); down_read(s); }
static inline int rwsem_is_locked(struct rw_semaphore *s)
{
	if (pthread_rwlock_trywrlock(&s->rw) == 0) { pthread_rwlock_unlock(&s->rw); return 0; }
	return 1;
}
#define down_read_nested(s, subclass) down_read(s)
#define down_write_nested(s, subclass) down_write(s)

#endif /* _LINUX_RWSEM_H */
