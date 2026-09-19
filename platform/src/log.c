/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Logging and BUG/WARN for the user-space port (linux/kernel.h, linux/bug.h).
 *
 * Every pr_xxx / printk / ntfs_debug call lands in platform_vlog(). With a sink
 * installed (the FSKit extension routes to os_log, ntfscli to stderr) the
 * sink gets every message and decides. Without one, messages at or above
 * NTFS_LOG_LEVEL (env, default "info") go to stderr.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <linux/kernel.h>
#include <linux/bug.h>

static platform_log_sink_t g_sink;
static void *g_sink_ctx;
static int g_stderr_level = -1;	/* resolved lazily from the environment */
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *const level_names[] = { "crit", "err", "warn", "info", "debug" };

static int stderr_level(void)
{
	int lvl = __atomic_load_n(&g_stderr_level, __ATOMIC_ACQUIRE);
	if (lvl >= 0)
		return lvl;
	lvl = PLATFORM_LOG_INFO;
	const char *env = getenv("NTFS_LOG_LEVEL");
	if (env) {
		unsigned i;
		for (i = 0; i < ARRAY_SIZE(level_names); i++)
			if (!strcasecmp(env, level_names[i]))
				lvl = (int)i;
		if (env[0] >= '0' && env[0] <= '9')
			lvl = atoi(env);
	}
	__atomic_store_n(&g_stderr_level, lvl, __ATOMIC_RELEASE);
	return lvl;
}

void platform_set_log_sink(platform_log_sink_t sink, void *ctx)
{
	pthread_mutex_lock(&g_log_lock);
	g_sink = sink;
	g_sink_ctx = ctx;
	pthread_mutex_unlock(&g_log_lock);
}

void platform_vlog(int level, const char *fmt, va_list ap)
{
	char buf[1024];
	size_t n;

	if (level < PLATFORM_LOG_CRIT)
		level = PLATFORM_LOG_CRIT;
	if (level > PLATFORM_LOG_DEBUG)
		level = PLATFORM_LOG_DEBUG;
	if (!g_sink && level > stderr_level())
		return;
	vsnprintf(buf, sizeof(buf), fmt, ap);
	n = strlen(buf);
	while (n && buf[n - 1] == '\n')
		buf[--n] = '\0';
	pthread_mutex_lock(&g_log_lock);
	if (g_sink)
		g_sink(level, buf, g_sink_ctx);
	else
		fprintf(stderr, "ntfs[%s]: %s\n", level_names[level], buf);
	pthread_mutex_unlock(&g_log_lock);
}

void platform_log(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	platform_vlog(level, fmt, ap);
	va_end(ap);
}

void platform_bug(const char *file, int line, const char *func, const char *expr)
{
	platform_log(PLATFORM_LOG_CRIT, "BUG at %s:%d %s(): %s", file, line, func, expr);
	fflush(stderr);
	abort();
}

int platform_warn(const char *file, int line, const char *func, const char *expr)
{
	platform_log(PLATFORM_LOG_WARN, "WARN at %s:%d %s(): %s", file, line, func, expr);
	return 1;
}
