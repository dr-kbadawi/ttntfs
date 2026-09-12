/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BUG_H
#define _LINUX_BUG_H

#include <linux/compiler.h>

void platform_bug(const char *file, int line, const char *func, const char *expr) __noreturn;
int platform_warn(const char *file, int line, const char *func, const char *expr);

#define BUG() platform_bug(__FILE__, __LINE__, __func__, "BUG")
#define BUG_ON(c) do { if (unlikely(c)) platform_bug(__FILE__, __LINE__, __func__, #c); } while (0)
#define WARN_ON(c) ({ int __r = !!(c); if (unlikely(__r)) platform_warn(__FILE__, __LINE__, __func__, #c); __r; })
#define WARN_ON_ONCE(c) WARN_ON(c)
#define WARN(c, fmt, ...) ({ int __r = !!(c); if (unlikely(__r)) platform_warn(__FILE__, __LINE__, __func__, fmt); __r; })
#define WARN_ONCE(c, fmt, ...) WARN(c, fmt, ##__VA_ARGS__)
#define VM_BUG_ON(c) BUG_ON(c)
#define VM_BUG_ON_FOLIO(c, f) BUG_ON(c)
#define VM_WARN_ON(c) WARN_ON(c)
#define VM_WARN_ON_ONCE(c) WARN_ON(c)
#define VM_WARN_ON_FOLIO(c, f) WARN_ON(c)

#endif /* _LINUX_BUG_H */
