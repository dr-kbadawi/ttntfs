/* SPDX-License-Identifier: GPL-2.0 */
/*
 * STANDALONE ONLY. Minimal platform_* symbols so platform/pagecache can be
 * built and tested before platform/src (compat stream) exists. Compiled by
 * platform/pagecache/CMakeLists.txt only; the top-level build must not
 * include this file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>

static platform_log_sink_t g_sink;
static void *g_sink_ctx;

void platform_set_log_sink(platform_log_sink_t sink, void *ctx) { g_sink = sink; g_sink_ctx = ctx; }
void platform_vlog(int level, const char *fmt, va_list ap)
{
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	if (g_sink) g_sink(level, buf, g_sink_ctx);
	else fprintf(stderr, "[pc%d] %s\n", level, buf);
}
void platform_log(int level, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); platform_vlog(level, fmt, ap); va_end(ap); }

void *platform_malloc(size_t size, gfp_t gfp)
{ void *p = malloc(size ? size : 1); if (p && (gfp & __GFP_ZERO)) memset(p, 0, size); return p; }
void platform_free(const void *p) { free((void *)p); }

void platform_bug(const char *file, int line, const char *func, const char *expr)
{ fprintf(stderr, "BUG at %s:%d %s(): %s\n", file, line, func, expr); abort(); }
int platform_warn(const char *file, int line, const char *func, const char *expr)
{ fprintf(stderr, "WARN at %s:%d %s(): %s\n", file, line, func, expr); return 1; }

struct timespec64 current_time(struct inode *inode)
{ struct timespec64 ts; (void)inode; ktime_get_coarse_real_ts64(&ts); return ts; }
