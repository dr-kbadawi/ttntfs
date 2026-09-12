/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STRING_H
#define _LINUX_STRING_H

#include <string.h>
#include <strings.h>
#include <linux/types.h>

#define unsafe_memcpy(dst, src, len, justification) memcpy(dst, src, len)
static inline size_t strlcpy_(char *dst, const char *src, size_t size) { return strlcpy(dst, src, size); }
size_t strscpy(char *dst, const char *src, size_t size);
static inline int strncasecmp_(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static inline void *memscan(void *addr, int c, size_t size)
{ unsigned char *p = addr; while (size) { if (*p == (unsigned char)c) return p; p++; size--; } return p; }
char *strreplace(char *s, char old, char new_);
static inline bool memchr_inv_zero(const void *p, size_t n)
{ const unsigned char *c = p; while (n--) if (*c++) return false; return true; }
void *memchr_inv(const void *s, int c, size_t n);
#define sysfs_streq(a, b) (strcmp(a, b) == 0)
#define strstarts(s, p) (strncmp(s, p, strlen(p)) == 0)

#endif /* _LINUX_STRING_H */
