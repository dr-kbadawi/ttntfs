/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Workqueues over one pthread per queue (linux/workqueue.h).
 *
 * A work item is either idle, pending (on the queue's list), or running.
 * queue_work() is idempotent while pending; flush_work() waits until the
 * item is neither pending nor running; cancel_work_sync() dequeues a
 * pending item and waits for a running one. system_wq is created on first
 * use.
 *
 * "Running" is recorded in the queue (wq->running), never in the item: as
 * in the kernel, a work function may free its own work_struct, so the
 * worker must not touch the item once the function has returned.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>

#define WORK_PENDING	1UL

struct workqueue_struct {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;		/* any state change */
	struct list_head pending;
	struct work_struct *running;
	bool stop;
	bool started;
	char name[32];
};

struct workqueue_struct *system_wq;
static pthread_once_t system_wq_once = PTHREAD_ONCE_INIT;

static void *worker_main(void *arg)
{
	struct workqueue_struct *wq = arg;

	pthread_setname_np(wq->name);
	pthread_mutex_lock(&wq->lock);
	for (;;) {
		struct work_struct *work;

		while (list_empty(&wq->pending) && !wq->stop)
			pthread_cond_wait(&wq->cond, &wq->lock);
		if (list_empty(&wq->pending) && wq->stop)
			break;
		work = list_first_entry(&wq->pending, struct work_struct, entry);
		list_del_init(&work->entry);
		work->state &= ~WORK_PENDING;
		wq->running = work;
		pthread_mutex_unlock(&wq->lock);

		work->func(work);
		/* @work may be gone now. */

		pthread_mutex_lock(&wq->lock);
		wq->running = NULL;
		pthread_cond_broadcast(&wq->cond);
	}
	pthread_mutex_unlock(&wq->lock);
	return NULL;
}

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...)
{
	struct workqueue_struct *wq = calloc(1, sizeof(*wq));
	va_list ap;

	(void)flags;
	(void)max_active;
	if (!wq)
		return NULL;
	va_start(ap, max_active);
	vsnprintf(wq->name, sizeof(wq->name), fmt ? fmt : "wq", ap);
	va_end(ap);
	pthread_mutex_init(&wq->lock, NULL);
	pthread_cond_init(&wq->cond, NULL);
	INIT_LIST_HEAD(&wq->pending);
	if (pthread_create(&wq->thread, NULL, worker_main, wq) != 0) {
		pthread_cond_destroy(&wq->cond);
		pthread_mutex_destroy(&wq->lock);
		free(wq);
		return NULL;
	}
	wq->started = true;
	return wq;
}

void flush_workqueue(struct workqueue_struct *wq)
{
	if (!wq)
		return;
	pthread_mutex_lock(&wq->lock);
	while (!list_empty(&wq->pending) || wq->running)
		pthread_cond_wait(&wq->cond, &wq->lock);
	pthread_mutex_unlock(&wq->lock);
}

void destroy_workqueue(struct workqueue_struct *wq)
{
	if (!wq)
		return;
	pthread_mutex_lock(&wq->lock);
	wq->stop = true;
	pthread_cond_broadcast(&wq->cond);
	pthread_mutex_unlock(&wq->lock);
	if (wq->started)
		pthread_join(wq->thread, NULL);
	pthread_cond_destroy(&wq->cond);
	pthread_mutex_destroy(&wq->lock);
	free(wq);
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	bool queued = false;

	if (!wq || !work)
		return false;
	pthread_mutex_lock(&wq->lock);
	if (!(work->state & WORK_PENDING) && !wq->stop) {
		work->state |= WORK_PENDING;
		work->wq = wq;
		list_add_tail(&work->entry, &wq->pending);
		pthread_cond_broadcast(&wq->cond);
		queued = true;
	}
	pthread_mutex_unlock(&wq->lock);
	return queued;
}

static void system_wq_init(void)
{
	system_wq = alloc_workqueue("events", 0, 0);
}

bool schedule_work(struct work_struct *work)
{
	pthread_once(&system_wq_once, system_wq_init);
	return queue_work(system_wq, work);
}

bool flush_work(struct work_struct *work)
{
	struct workqueue_struct *wq;
	bool waited = false;

	if (!work || !(wq = work->wq))
		return false;
	pthread_mutex_lock(&wq->lock);
	while ((work->state & WORK_PENDING) || wq->running == work) {
		waited = true;
		pthread_cond_wait(&wq->cond, &wq->lock);
	}
	pthread_mutex_unlock(&wq->lock);
	return waited;
}

bool cancel_work_sync(struct work_struct *work)
{
	struct workqueue_struct *wq;
	bool was_pending = false;

	if (!work || !(wq = work->wq))
		return false;
	pthread_mutex_lock(&wq->lock);
	if (work->state & WORK_PENDING) {
		list_del_init(&work->entry);
		work->state &= ~WORK_PENDING;
		was_pending = true;
	}
	while (wq->running == work)
		pthread_cond_wait(&wq->cond, &wq->lock);
	pthread_mutex_unlock(&wq->lock);
	return was_pending;
}
