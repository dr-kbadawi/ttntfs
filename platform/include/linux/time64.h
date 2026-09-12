/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TIME64_H
#define _LINUX_TIME64_H

#include <time.h>
#include <linux/types.h>

#define NSEC_PER_SEC	1000000000L
#define NSEC_PER_MSEC	1000000L
#define NSEC_PER_USEC	1000L
#define USEC_PER_SEC	1000000L
#define MSEC_PER_SEC	1000L
#define TIME64_MAX	((s64)~((u64)1 << 63))
#define KTIME_MAX	TIME64_MAX

typedef s64 time64_t;

static inline int timespec64_equal(const struct timespec64 *a, const struct timespec64 *b)
{ return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec; }
static inline int timespec64_compare(const struct timespec64 *lhs, const struct timespec64 *rhs)
{
	if (lhs->tv_sec < rhs->tv_sec) return -1;
	if (lhs->tv_sec > rhs->tv_sec) return 1;
	return (int)(lhs->tv_nsec - rhs->tv_nsec);
}
static inline s64 timespec64_to_ns(const struct timespec64 *ts) { return ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec; }
static inline struct timespec64 ns_to_timespec64(s64 ns)
{
	struct timespec64 ts = { ns / NSEC_PER_SEC, ns % NSEC_PER_SEC };
	if (ts.tv_nsec < 0) { ts.tv_nsec += NSEC_PER_SEC; ts.tv_sec--; }
	return ts;
}
static inline bool timespec64_valid(const struct timespec64 *ts)
{ return ts->tv_sec >= 0 && (unsigned long)ts->tv_nsec < NSEC_PER_SEC; }
struct timespec64 timespec64_trunc(struct timespec64 t, unsigned int gran);

static inline void ktime_get_coarse_real_ts64(struct timespec64 *ts)
{ struct timespec t; clock_gettime(CLOCK_REALTIME, &t); ts->tv_sec = t.tv_sec; ts->tv_nsec = t.tv_nsec; }
static inline void ktime_get_real_ts64(struct timespec64 *ts) { ktime_get_coarse_real_ts64(ts); }
static inline s64 ktime_get_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (s64)t.tv_sec * NSEC_PER_SEC + t.tv_nsec; }
static inline s64 ktime_get_real_seconds(void) { return (s64)time(NULL); }
#define ktime_get() ktime_get_ns()
static inline unsigned long jiffies_(void) { return (unsigned long)(ktime_get_ns() / NSEC_PER_MSEC); }
#define jiffies jiffies_()
#define HZ 1000
#define msecs_to_jiffies(m) ((unsigned long)(m))
#define jiffies_to_msecs(j) ((unsigned int)(j))
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)

#endif /* _LINUX_TIME64_H */
