/* SPDX-License-Identifier: GPL-2.0 */
/* Small self-contained SHA-256 (FIPS 180-4), shared by mkfixtures and ntfscli. */
#ifndef TOOLS_SHA256_H
#define TOOLS_SHA256_H
#include <stdint.h>
#include <stddef.h>

struct sha256_ctx {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	size_t buflen;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, uint8_t out[32]);
/* Convenience: digest -> 64 lowercase hex chars + NUL into hex[65]. */
void sha256_hex(const uint8_t digest[32], char hex[65]);
void sha256_buf_hex(const void *data, size_t len, char hex[65]);

#endif
