/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Wait queues (linux/wait.h): a mutex + condition variable. wait_event()
 * evaluates its condition with the mutex held and blocks in __wait_block();
 * wake_up_all() broadcasts under the same mutex, so a state change followed
 * by a wake-up cannot slip between the check and the wait. The block has a
 * safety timeout so a caller that forgets wake_up() degrades to polling.
 */
#include <errno.h>
#include <time.h>
#include <linux/wait.h>

#define WAIT_POLL_NS	100000000L	/* 100 ms */

void init_waitqueue_head(wait_queue_head_t *q)
{
	pthread_mutex_init(&q->m, NULL);
	pthread_cond_init(&q->c, NULL);
}

void destroy_waitqueue_head(wait_queue_head_t *q)
{
	pthread_cond_destroy(&q->c);
	pthread_mutex_destroy(&q->m);
}

void wake_up_all(wait_queue_head_t *q)
{
	pthread_mutex_lock(&q->m);
	pthread_cond_broadcast(&q->c);
	pthread_mutex_unlock(&q->m);
}

void __wait_lock(wait_queue_head_t *q)
{
	pthread_mutex_lock(&q->m);
}

void __wait_block(wait_queue_head_t *q)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += WAIT_POLL_NS;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait(&q->c, &q->m, &ts);
}

void __wait_unlock(wait_queue_head_t *q)
{
	pthread_mutex_unlock(&q->m);
}
