/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Wait queues over a pthread mutex + condition variable. Wakers broadcast
 * under the queue mutex, and wait_event() re-checks the condition under the
 * same mutex, so a state change followed by wake_up() is never lost (the
 * kernel demands the same ordering). Implemented in platform/src/wait.c.
 */
#ifndef _LINUX_WAIT_H
#define _LINUX_WAIT_H
#include <pthread.h>
#include <linux/types.h>
typedef struct { pthread_mutex_t m; pthread_cond_t c; } wait_queue_head_t;
#define DECLARE_WAIT_QUEUE_HEAD(n) wait_queue_head_t n = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER }
void init_waitqueue_head(wait_queue_head_t *q);
void destroy_waitqueue_head(wait_queue_head_t *q);
void wake_up_all(wait_queue_head_t *q);
#define wake_up(q) wake_up_all(q)
#define wake_up_interruptible(q) wake_up_all(q)
#define wake_up_interruptible_all(q) wake_up_all(q)
/* Internals of wait_event(): lock, block (with a safety timeout so a missed
 * wake-up degrades to a poll rather than a hang), unlock. */
void __wait_lock(wait_queue_head_t *q);
void __wait_block(wait_queue_head_t *q);
void __wait_unlock(wait_queue_head_t *q);
#define wait_event(q, cond) do { \
	__wait_lock(&(q)); \
	while (!(cond)) __wait_block(&(q)); \
	__wait_unlock(&(q)); } while (0)
#define wait_event_interruptible(q, cond) ({ wait_event(q, cond); 0; })
#define wait_event_killable(q, cond) ({ wait_event(q, cond); 0; })
#endif
