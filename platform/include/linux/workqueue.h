/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WORKQUEUE_H
#define _LINUX_WORKQUEUE_H
#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);
struct work_struct { struct list_head entry; work_func_t func; unsigned long state; struct workqueue_struct *wq; };
struct workqueue_struct;
#define WQ_UNBOUND (1 << 1)
#define WQ_MEM_RECLAIM (1 << 3)
#define WQ_HIGHPRI (1 << 4)
#define WQ_FREEZABLE (1 << 2)
#define INIT_WORK(w, f) do { INIT_LIST_HEAD(&(w)->entry); (w)->func = (f); (w)->state = 0; (w)->wq = NULL; } while (0)
struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...);
void destroy_workqueue(struct workqueue_struct *wq);
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool schedule_work(struct work_struct *work);
bool flush_work(struct work_struct *work);
bool cancel_work_sync(struct work_struct *work);
void flush_workqueue(struct workqueue_struct *wq);
extern struct workqueue_struct *system_wq;
#endif
