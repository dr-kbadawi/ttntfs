/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * seq_file: a bounded text buffer. super_operations.show_options writes the
 * mount options into it; the vfs layer can hand one to the core to render
 * them for ntfs_volume_info. Implemented in platform/src/misc.c.
 */
#ifndef _LINUX_SEQ_FILE_H
#define _LINUX_SEQ_FILE_H
#include <stddef.h>
#include <linux/types.h>
struct seq_file {
	char *buf;
	size_t size;		/* capacity */
	size_t count;		/* bytes used */
	bool overflow;
};
static inline void seq_init(struct seq_file *m, char *buf, size_t size)
{ m->buf = buf; m->size = size; m->count = 0; m->overflow = false; if (size) buf[0] = '\0'; }
void seq_printf(struct seq_file *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void seq_puts(struct seq_file *m, const char *s);
void seq_putc(struct seq_file *m, char c);
static inline bool seq_has_overflowed(const struct seq_file *m) { return m->overflow; }
#endif
