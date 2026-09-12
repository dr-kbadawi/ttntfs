/* SPDX-License-Identifier: GPL-2.0 */
/* Minimal iov_iter: a single contiguous user buffer. compress.c's buffered
 * write path consumes it; the vfs layer builds one per ntfs_write() call. */
#ifndef _LINUX_UIO_H
#define _LINUX_UIO_H
#include <linux/types.h>
#include <linux/mm_types.h>
#include <string.h>
struct iov_iter {
	const char *buf;	/* ITER_SOURCE */
	char *wbuf;		/* ITER_DEST */
	size_t count;		/* bytes remaining */
	size_t off;		/* consumed */
};
#define ITER_SOURCE 1
#define ITER_DEST 0
static inline void iov_iter_init_src(struct iov_iter *i, const void *buf, size_t count) { i->buf = buf; i->wbuf = NULL; i->count = count; i->off = 0; }
static inline void iov_iter_init_dst(struct iov_iter *i, void *buf, size_t count) { i->buf = NULL; i->wbuf = buf; i->count = count; i->off = 0; }
static inline size_t iov_iter_count(const struct iov_iter *i) { return i->count; }
static inline void iov_iter_advance(struct iov_iter *i, size_t n) { if (n > i->count) n = i->count; i->off += n; i->count -= n; }
static inline void iov_iter_revert(struct iov_iter *i, size_t n) { i->off -= n; i->count += n; }
static inline void iov_iter_truncate(struct iov_iter *i, u64 count) { if (i->count > count) i->count = count; }
static inline size_t fault_in_iov_iter_readable(const struct iov_iter *i, size_t size) { (void)i; (void)size; return 0; }
static inline size_t fault_in_iov_iter_writeable(const struct iov_iter *i, size_t size) { (void)i; (void)size; return 0; }
static inline size_t copy_from_iter(void *to, size_t bytes, struct iov_iter *i)
{ if (bytes > i->count) bytes = i->count; memcpy(to, i->buf + i->off, bytes); iov_iter_advance(i, bytes); return bytes; }
static inline size_t copy_to_iter(const void *from, size_t bytes, struct iov_iter *i)
{ if (bytes > i->count) bytes = i->count; memcpy(i->wbuf + i->off, from, bytes); iov_iter_advance(i, bytes); return bytes; }
static inline size_t copy_folio_from_iter_atomic(struct folio *folio, size_t offset, size_t bytes, struct iov_iter *i)
{ return copy_from_iter((char *)folio->data + offset, bytes, i); }
static inline size_t copy_page_from_iter_atomic(struct page *page, size_t offset, size_t bytes, struct iov_iter *i)
{ return copy_from_iter((char *)page_folio(page)->data + offset, bytes, i); }
#endif
