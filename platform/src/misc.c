/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Small kernel library functions: string parsing (kstrto*, simple_strto*),
 * strscpy, hex helpers, sort(), time truncation, seq_file, and the generic_*
 * file operations the core's tables reference.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/sort.h>
#include <linux/time64.h>
#include <linux/fs.h>
#include <linux/seq_file.h>

/* ------------------------------------------------------------------ */
/* Number parsing                                                      */

unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base)
{
	return strtoul(cp, endp, (int)base);
}

long simple_strtol(const char *cp, char **endp, unsigned int base)
{
	return strtol(cp, endp, (int)base);
}

unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base)
{
	return strtoull(cp, endp, (int)base);
}

/* Kernel kstrto*: the whole string must be a number (one trailing newline
 * is tolerated); -EINVAL otherwise, -ERANGE on overflow. */
static int parse_ull(const char *s, unsigned int base, unsigned long long *out, bool *neg)
{
	char *end;
	unsigned long long v;

	if (!s || !*s)
		return -EINVAL;
	*neg = false;
	if (*s == '-') {
		*neg = true;
		s++;
	} else if (*s == '+') {
		s++;
	}
	if (!isdigit((unsigned char)*s) && !(base == 16 || base == 0) )
		return -EINVAL;
	if (*s == '-' || *s == '+' || isspace((unsigned char)*s))
		return -EINVAL;
	errno = 0;
	v = strtoull(s, &end, (int)base);
	if (end == s)
		return -EINVAL;
	if (errno == ERANGE)
		return -ERANGE;
	if (*end == '\n')
		end++;
	if (*end)
		return -EINVAL;
	*out = v;
	return 0;
}

int kstrtou64(const char *s, unsigned int base, u64 *res)
{
	unsigned long long v;
	bool neg;
	int err = parse_ull(s, base, &v, &neg);

	if (err)
		return err;
	if (neg)
		return -EINVAL;
	*res = v;
	return 0;
}

int kstrtoul(const char *s, unsigned int base, unsigned long *res)
{
	u64 v;
	int err = kstrtou64(s, base, &v);

	if (err)
		return err;
	*res = (unsigned long)v;
	return 0;
}

int kstrtou32(const char *s, unsigned int base, u32 *res)
{
	u64 v;
	int err = kstrtou64(s, base, &v);

	if (err)
		return err;
	if (v > U32_MAX)
		return -ERANGE;
	*res = (u32)v;
	return 0;
}

int kstrtos32(const char *s, unsigned int base, s32 *res)
{
	unsigned long long v;
	bool neg;
	int err = parse_ull(s, base, &v, &neg);

	if (err)
		return err;
	if (neg) {
		if (v > (unsigned long long)S32_MAX + 1)
			return -ERANGE;
		*res = (s32)(0 - v);
	} else {
		if (v > (unsigned long long)S32_MAX)
			return -ERANGE;
		*res = (s32)v;
	}
	return 0;
}

int kstrtoint(const char *s, unsigned int base, int *res)
{
	return kstrtos32(s, base, res);
}

int kstrtobool(const char *s, bool *res)
{
	if (!s)
		return -EINVAL;
	switch (s[0]) {
	case 'y': case 'Y': case 't': case 'T': case '1':
		*res = true;
		return 0;
	case 'n': case 'N': case 'f': case 'F': case '0':
		*res = false;
		return 0;
	case 'o': case 'O':
		switch (s[1]) {
		case 'n': case 'N':
			*res = true;
			return 0;
		case 'f': case 'F':
			*res = false;
			return 0;
		}
		break;
	}
	return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Strings                                                             */

size_t strscpy(char *dst, const char *src, size_t size)
{
	size_t len;

	if (!size)
		return (size_t)-E2BIG;
	len = strnlen(src, size);
	if (len == size) {
		memcpy(dst, src, size - 1);
		dst[size - 1] = '\0';
		return (size_t)-E2BIG;
	}
	memcpy(dst, src, len + 1);
	return len;
}

char *strreplace(char *s, char old, char new_)
{
	char *p = s;

	for (; *p; p++)
		if (*p == old)
			*p = new_;
	return p;
}

void *memchr_inv(const void *s, int c, size_t n)
{
	const unsigned char *p = s;

	while (n--) {
		if (*p != (unsigned char)c)
			return (void *)p;
		p++;
	}
	return NULL;
}

const char hex_asc[] = "0123456789abcdef";
const char hex_asc_upper[] = "0123456789ABCDEF";

int hex_to_bin(unsigned char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	ch |= 0x20;
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	return -1;
}

/* ------------------------------------------------------------------ */
/* sort(): heapsort, like lib/sort.c. Not stable, no allocation.       */

static void generic_swap(void *a, void *b, int size)
{
	char *x = a, *y = b;

	while (size-- > 0) {
		char t = *x;
		*x++ = *y;
		*y++ = t;
	}
}

void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
	  void (*swap_fn)(void *, void *, int))
{
	char *b = base;
	size_t i, c, r, n = num;

	if (!swap_fn)
		swap_fn = generic_swap;
	if (n < 2)
		return;
	/* heapify */
	for (i = (n / 2) * size; i > 0; ) {
		i -= size;
		for (r = i; r * 2 + size < n * size; r = c) {
			c = r * 2 + size;
			if (c + size < n * size && cmp(b + c, b + c + size) < 0)
				c += size;
			if (cmp(b + r, b + c) >= 0)
				break;
			swap_fn(b + r, b + c, (int)size);
		}
	}
	/* sort */
	for (i = (n - 1) * size; i > 0; i -= size) {
		swap_fn(b, b + i, (int)size);
		for (r = 0; r * 2 + size < i; r = c) {
			c = r * 2 + size;
			if (c + size < i && cmp(b + c, b + c + size) < 0)
				c += size;
			if (cmp(b + r, b + c) >= 0)
				break;
			swap_fn(b + r, b + c, (int)size);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Time                                                                */

struct timespec64 timespec64_trunc(struct timespec64 t, unsigned int gran)
{
	if (gran == 1) {
		/* nothing */
	} else if (gran == NSEC_PER_SEC) {
		t.tv_nsec = 0;
	} else if (gran > 1 && gran < NSEC_PER_SEC) {
		t.tv_nsec -= t.tv_nsec % gran;
	} else {
		WARN(1, "illegal file time granularity: %u", gran);
	}
	return t;
}

struct timespec64 current_time(struct inode *inode)
{
	struct timespec64 now;

	ktime_get_coarse_real_ts64(&now);
	if (!inode || !inode->i_sb)
		return now;
	return timespec64_trunc(now, inode->i_sb->s_time_gran);
}

/* ------------------------------------------------------------------ */
/* seq_file                                                            */

void seq_printf(struct seq_file *m, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!m->buf || m->count >= m->size) {
		m->overflow = true;
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(m->buf + m->count, m->size - m->count, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= m->size - m->count) {
		m->overflow = true;
		m->count = m->size - 1;
		m->buf[m->count] = '\0';
		return;
	}
	m->count += (size_t)n;
}

void seq_puts(struct seq_file *m, const char *s)
{
	seq_printf(m, "%s", s);
}

void seq_putc(struct seq_file *m, char c)
{
	seq_printf(m, "%c", c);
}

/* ------------------------------------------------------------------ */
/* generic_* file operations referenced by the core's tables           */

loff_t generic_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_inode;
	loff_t pos;

	switch (whence) {
	case SEEK_SET:
		pos = offset;
		break;
	case SEEK_CUR:
		pos = file->f_pos + offset;
		break;
	case SEEK_END:
		pos = i_size_read(inode) + offset;
		break;
	default:
		return -EINVAL;
	}
	if (pos < 0 || pos > inode->i_sb->s_maxbytes)
		return -EINVAL;
	file->f_pos = pos;
	return pos;
}

ssize_t generic_read_dir(struct file *filp, char *buf, size_t siz, loff_t *ppos)
{
	(void)filp; (void)buf; (void)siz; (void)ppos;
	return -EISDIR;
}

int generic_setlease(struct file *filp, int arg, void **flp, void **priv)
{
	(void)filp; (void)arg; (void)flp; (void)priv;
	return -EINVAL;
}
