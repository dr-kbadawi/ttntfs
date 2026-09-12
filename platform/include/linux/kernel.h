/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KERNEL_H
#define _LINUX_KERNEL_H

#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/bug.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/log2.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/overflow.h>

struct va_format { const char *fmt; va_list *va; };

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define __ALIGN_KERNEL_MASK(x, mask) (((x) + (mask)) & ~(mask))
#define __ALIGN_KERNEL(x, a) __ALIGN_KERNEL_MASK(x, (__typeof__(x))(a) - 1)
#define ALIGN(x, a) __ALIGN_KERNEL((x), (a))
#define ALIGN_DOWN(x, a) __ALIGN_KERNEL((x) - ((a) - 1), (a))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a) ((__typeof__(p))ALIGN((unsigned long)(p), (a)))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_DOWN_ULL(ll, d) ((unsigned long long)(ll) / (d))
#define DIV_ROUND_UP_ULL(ll, d) DIV_ROUND_DOWN_ULL((unsigned long long)(ll) + (d) - 1, (d))
#define roundup(x, y) ({ __typeof__(y) __y = (y); (((x) + (__y - 1)) / __y) * __y; })
#define rounddown(x, y) ({ __typeof__(x) __x = (x); __x - (__x % (y)); })
#define round_up(x, y) ((((x) - 1) | ((__typeof__(x))(y) - 1)) + 1)
#define round_down(x, y) ((x) & ~((__typeof__(x))(y) - 1))

#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(type, a, b) min((type)(a), (type)(b))
#define max_t(type, a, b) max((type)(a), (type)(b))
#define min3(a, b, c) min(min(a, b), c)
#define max3(a, b, c) max(max(a, b), c)
#define clamp(val, lo, hi) min(max(val, lo), hi)
#define clamp_t(type, val, lo, hi) min_t(type, max_t(type, val, lo), hi)
#define clamp_val(val, lo, hi) clamp_t(__typeof__(val), val, lo, hi)
#define swap(a, b) do { __typeof__(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)
#define cmp_int(a, b) ((a) > (b) ? 1 : (a) < (b) ? -1 : 0)

#define container_of(ptr, type, member) ({ \
	const __typeof__(((type *)0)->member) *__mptr = (ptr); \
	(type *)((char *)__mptr - offsetof(type, member)); })
#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))
#define offsetofend(TYPE, MEMBER) (offsetof(TYPE, MEMBER) + sizeof_field(TYPE, MEMBER))

#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))

#define U8_MAX  ((u8)~0U)
#define S8_MAX  ((s8)(U8_MAX >> 1))
#define U16_MAX ((u16)~0U)
#define S16_MAX ((s16)(U16_MAX >> 1))
#define U32_MAX ((u32)~0U)
#define S32_MAX ((s32)(U32_MAX >> 1))
#define S32_MIN ((s32)(-S32_MAX - 1))
#define U64_MAX ((u64)~0ULL)
#define S64_MAX ((s64)(U64_MAX >> 1))
#define S64_MIN ((s64)(-S64_MAX - 1))

/* Logging: everything goes through the platform log sink. */
enum { PLATFORM_LOG_CRIT = 0, PLATFORM_LOG_ERR, PLATFORM_LOG_WARN, PLATFORM_LOG_INFO, PLATFORM_LOG_DEBUG };
void platform_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void platform_vlog(int level, const char *fmt, va_list ap);
typedef void (*platform_log_sink_t)(int level, const char *msg, void *ctx);
void platform_set_log_sink(platform_log_sink_t sink, void *ctx);

#define KERN_SOH	""
#define KERN_EMERG	""
#define KERN_ALERT	""
#define KERN_CRIT	""
#define KERN_ERR	""
#define KERN_WARNING	""
#define KERN_NOTICE	""
#define KERN_INFO	""
#define KERN_DEBUG	""
#define KERN_CONT	""

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif
#define printk(fmt, ...) platform_log(PLATFORM_LOG_INFO, fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...) platform_log(PLATFORM_LOG_CRIT, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...) platform_log(PLATFORM_LOG_ERR, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...) platform_log(PLATFORM_LOG_WARN, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice(fmt, ...) platform_log(PLATFORM_LOG_INFO, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...) platform_log(PLATFORM_LOG_INFO, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...) platform_log(PLATFORM_LOG_DEBUG, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err_ratelimited pr_err
#define pr_warn_ratelimited pr_warn
#define pr_info_ratelimited pr_info
#define no_printk(fmt, ...) do { if (0) platform_log(PLATFORM_LOG_DEBUG, fmt, ##__VA_ARGS__); } while (0)
#define printk_ratelimited printk
#define panic(fmt, ...) do { platform_log(PLATFORM_LOG_CRIT, fmt, ##__VA_ARGS__); __builtin_trap(); } while (0)

/* Scheduling hints: nothing to do in user space. */
#define might_sleep() do { } while (0)
#define might_sleep_if(c) do { } while (0)
#define cond_resched() do { } while (0)
#define cpu_relax() do { } while (0)
#define schedule() do { } while (0)
#define signal_pending(t) 0
#define fatal_signal_pending(t) 0
#define current ((void *)0)
#define in_interrupt() 0
#define preempt_disable() do { } while (0)
#define preempt_enable() do { } while (0)
#define lockdep_off() do { } while (0)
#define lockdep_on() do { } while (0)
#define lockdep_set_class(l, k) do { (void)(l); (void)(k); } while (0)
/* Lockdep keys are plain (unused) objects here; complete so file-scope
 * "static struct lock_class_key k;" definitions are legal. */
struct lock_class_key { unsigned char dummy; };
#define lockdep_assert_held(l) do { } while (0)
#define rcu_barrier() do { } while (0)
#define synchronize_rcu() do { } while (0)
#define totalram_pages() ((unsigned long)-1 >> PAGE_SHIFT)

/* Kernel-style simple_strtoul & friends. */
unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base);
long simple_strtol(const char *cp, char **endp, unsigned int base);
unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base);
int kstrtoul(const char *s, unsigned int base, unsigned long *res);
int kstrtoint(const char *s, unsigned int base, int *res);
int kstrtou32(const char *s, unsigned int base, u32 *res);
int kstrtos32(const char *s, unsigned int base, s32 *res);
int kstrtou64(const char *s, unsigned int base, u64 *res);
int kstrtobool(const char *s, bool *res);

#define hex_asc_lo(x) hex_asc[((x) & 0x0f)]
#define hex_asc_hi(x) hex_asc[((x) & 0xf0) >> 4]
extern const char hex_asc[];
extern const char hex_asc_upper[];
int hex_to_bin(unsigned char ch);

#endif /* _LINUX_KERNEL_H */
