/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * UTF-8 <-> UTF-16 conversion and the fixed UTF-8 NLS table
 * (linux/nls.h). Same behaviour as the kernel's fs/nls/nls_base.c:
 *  - utf8s_to_utf16s() stops at NUL or at the output limit, encodes code
 *    points above the BMP as surrogate pairs, and returns -EINVAL on an
 *    invalid UTF-8 sequence;
 *  - utf16s_to_utf8s() stops at NUL or the output limit and silently skips
 *    unpaired surrogates.
 */
#include <asm/byteorder.h>
#include <string.h>
#include <strings.h>
#include <linux/nls.h>
#include <linux/errno.h>

#define UNICODE_MAX	0x0010ffff
#define PLANE_SIZE	0x00010000
#define SURROGATE_MASK	0xfffff800
#define SURROGATE_PAIR	0x0000d800
#define SURROGATE_LOW	0x00000400
#define SURROGATE_BITS	0x000003ff

struct utf8_table {
	int cmask;
	int cval;
	int shift;
	long lmask;
	long lval;
};

static const struct utf8_table utf8_table[] = {
	{ 0x80, 0x00, 0 * 6, 0x7F,       0,         },	/* 1 byte sequence */
	{ 0xE0, 0xC0, 1 * 6, 0x7FF,      0x80,      },	/* 2 byte sequence */
	{ 0xF0, 0xE0, 2 * 6, 0xFFFF,     0x800,     },	/* 3 byte sequence */
	{ 0xF8, 0xF0, 3 * 6, 0x1FFFFF,   0x10000,   },	/* 4 byte sequence */
	{ 0xFC, 0xF8, 4 * 6, 0x3FFFFFF,  0x200000,  },	/* 5 byte sequence */
	{ 0xFE, 0xFC, 5 * 6, 0x7FFFFFFF, 0x4000000, },	/* 6 byte sequence */
	{ 0, 0, 0, 0, 0 }
};

int utf8_to_utf32(const u8 *s, int inlen, u32 *pu)
{
	unsigned long l;
	int c0, c, nc;
	const struct utf8_table *t;

	nc = 0;
	c0 = *s;
	l = c0;
	for (t = utf8_table; t->cmask; t++) {
		nc++;
		if ((c0 & t->cmask) == t->cval) {
			l &= t->lmask;
			if (l < (unsigned long)t->lval || l > UNICODE_MAX ||
			    (l & SURROGATE_MASK) == SURROGATE_PAIR)
				return -1;
			*pu = (u32)l;
			return nc;
		}
		if (inlen <= nc)
			return -1;
		s++;
		c = (*s ^ 0x80) & 0xFF;
		if (c & 0xC0)
			return -1;
		l = (l << 6) | c;
	}
	return -1;
}

int utf32_to_utf8(u32 u, u8 *s, int maxout)
{
	unsigned long l;
	int c, nc;
	const struct utf8_table *t;

	if (!s)
		return 0;
	l = u;
	if (l > UNICODE_MAX || (l & SURROGATE_MASK) == SURROGATE_PAIR)
		return -1;
	nc = 0;
	for (t = utf8_table; t->cmask && maxout; t++, maxout--) {
		nc++;
		if (l <= (unsigned long)t->lmask) {
			c = t->shift;
			*s = (u8)(t->cval | (l >> c));
			while (c > 0) {
				c -= 6;
				s++;
				*s = (u8)(0x80 | ((l >> c) & 0x3F));
			}
			return nc;
		}
	}
	return -1;
}

static inline void put_utf16(__le16 *s, unsigned int c, enum utf16_endian endian)
{
	switch (endian) {
	default:
		*s = (u16)c;
		break;
	case UTF16_LITTLE_ENDIAN:
		*s = cpu_to_le16((u16)c);
		break;
	case UTF16_BIG_ENDIAN:
		*s = (u16)__builtin_bswap16((u16)c);
		break;
	}
}

static inline unsigned long get_utf16(unsigned int c, enum utf16_endian endian)
{
	switch (endian) {
	default:
		return c;
	case UTF16_LITTLE_ENDIAN:
		return le16_to_cpu((u16)c);
	case UTF16_BIG_ENDIAN:
		return __builtin_bswap16((u16)c);
	}
}

int utf8s_to_utf16s(const u8 *s, int inlen, enum utf16_endian endian, __le16 *pwcs, int maxout)
{
	__le16 *op = pwcs;
	int size;
	u32 u;

	while (inlen > 0 && maxout > 0 && *s) {
		if (*s & 0x80) {
			size = utf8_to_utf32(s, inlen, &u);
			if (size < 0)
				return -EINVAL;
			s += size;
			inlen -= size;
			if (u >= PLANE_SIZE) {
				if (maxout < 2)
					break;
				u -= PLANE_SIZE;
				put_utf16(op++, SURROGATE_PAIR | ((u >> 10) & SURROGATE_BITS), endian);
				put_utf16(op++, SURROGATE_PAIR | SURROGATE_LOW | (u & SURROGATE_BITS), endian);
				maxout -= 2;
			} else {
				put_utf16(op++, u, endian);
				maxout--;
			}
		} else {
			put_utf16(op++, *s++, endian);
			inlen--;
			maxout--;
		}
	}
	return (int)(op - pwcs);
}

int utf16s_to_utf8s(const __le16 *pwcs, int inlen, enum utf16_endian endian, u8 *s, int maxout)
{
	u8 *op = s;
	int size;
	unsigned long u, v;

	while (inlen > 0 && maxout > 0) {
		u = get_utf16(*pwcs, endian);
		if (!u)
			break;
		pwcs++;
		inlen--;
		if (u > 0x7f) {
			if ((u & SURROGATE_MASK) == SURROGATE_PAIR) {
				if (u & SURROGATE_LOW)
					continue;	/* unpaired low: skip */
				if (inlen <= 0)
					break;
				v = get_utf16(*pwcs, endian);
				if ((v & SURROGATE_MASK) != SURROGATE_PAIR || !(v & SURROGATE_LOW))
					continue;	/* unpaired high: skip */
				u = PLANE_SIZE + ((u & SURROGATE_BITS) << 10) + (v & SURROGATE_BITS);
				pwcs++;
				inlen--;
			}
			size = utf32_to_utf8((u32)u, op, maxout);
			if (size == -1) {
				/* Ignore character and move on */
			} else {
				op += size;
				maxout -= size;
			}
		} else {
			*op++ = (u8)u;
			maxout--;
		}
	}
	return (int)(op - s);
}

/* ------------------------------------------------------------------ */
/* NLS table: the port always speaks UTF-8.                            */

static int utf8_uni2char(wchar_t_nls uni, unsigned char *out, int boundlen)
{
	int n;

	if (boundlen <= 0)
		return -ENAMETOOLONG;
	n = utf32_to_utf8(uni, out, boundlen);
	if (n < 0) {
		*out = '?';
		return -EINVAL;
	}
	return n;
}

static int utf8_char2uni(const unsigned char *rawstring, int boundlen, wchar_t_nls *uni)
{
	int n;
	u32 u;

	n = utf8_to_utf32(rawstring, boundlen, &u);
	if (n < 0 || u > 0xffff) {
		*uni = 0x003f;	/* '?' */
		return -EINVAL;
	}
	*uni = (wchar_t_nls)u;
	return n;
}

static struct nls_table utf8_table_nls = {
	.charset = "utf8",
	.uni2char = utf8_uni2char,
	.char2uni = utf8_char2uni,
};

struct nls_table *load_nls(const char *charset)
{
	if (!charset || !strcasecmp(charset, "utf8") || !strcasecmp(charset, "utf-8"))
		return &utf8_table_nls;
	return NULL;
}

struct nls_table *load_nls_default(void)
{
	return &utf8_table_nls;
}

void unload_nls(struct nls_table *t)
{
	(void)t;
}
