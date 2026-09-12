/* SPDX-License-Identifier: GPL-2.0 */
/* The port always speaks UTF-8; nls_table is a fixed UTF-8 codec. */
#ifndef _LINUX_NLS_H
#define _LINUX_NLS_H
#include <linux/types.h>
/* The kernel's nls.h defines wchar_t as a 16-bit UTF-16 unit and the driver
 * relies on that (unistr.c casts __le16 buffers to wchar_t *). Darwin's
 * wchar_t is 32-bit, so after the common system headers have been seen with
 * the host type, the name is redirected to the 16-bit one for driver code. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <wchar.h>
#include <time.h>
#include <pthread.h>
typedef u16 wchar_t_nls;
#define wchar_t wchar_t_nls
struct nls_table {
	const char *charset;
	int (*uni2char)(wchar_t_nls uni, unsigned char *out, int boundlen);
	int (*char2uni)(const unsigned char *rawstring, int boundlen, wchar_t_nls *uni);
};
enum utf16_endian { UTF16_HOST_ENDIAN, UTF16_LITTLE_ENDIAN, UTF16_BIG_ENDIAN };
#define NLS_MAX_CHARSET_SIZE 6
struct nls_table *load_nls(const char *charset);
struct nls_table *load_nls_default(void);
void unload_nls(struct nls_table *t);
int utf8s_to_utf16s(const u8 *s, int len, enum utf16_endian endian, __le16 *pwcs, int maxlen);
int utf16s_to_utf8s(const __le16 *pwcs, int len, enum utf16_endian endian, u8 *s, int maxlen);
int utf8_to_utf32(const u8 *s, int len, u32 *pu);
int utf32_to_utf8(u32 u, u8 *s, int maxlen);
#endif
