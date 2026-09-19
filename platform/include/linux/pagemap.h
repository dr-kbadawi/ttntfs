/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Page cache API for the user-space port. CONTRACT HEADER.
 *
 * Implemented in platform/pagecache/. Semantics follow the kernel closely
 * because the ported core (mft.c, attrib.c, lcnalloc.c, bitmap.c, ...) relies
 * on them for metadata correctness:
 *
 *  - read_mapping_folio() returns a referenced, unlocked, uptodate folio
 *    (or ERR_PTR). It reads through mapping->a_ops->read_folio on a miss.
 *  - folio_lock()/folio_unlock() are a real mutex. Nesting: a folio lock
 *    is taken outside all ntfs inode/runlist locks (same as the kernel).
 *  - folio_mark_dirty() queues the folio for write-back. The write-back
 *    thread flushes dirty folios older than NTFS_WRITEBACK_INTERVAL_MS
 *    through a_ops->write_folio, and filemap_write_and_wait*() flush
 *    synchronously.
 *  - Reference counting: read_mapping_folio/__filemap_get_folio return with
 *    a reference; folio_put() drops it. A folio with zero references and not
 *    dirty is reclaimable under memory pressure (NTFS_PAGECACHE_MAX_FOLIOS).
 */
#ifndef _LINUX_PAGEMAP_H
#define _LINUX_PAGEMAP_H

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/err.h>
#include <linux/bitops.h>
#include <linux/highmem.h>

/* FGP flags for __filemap_get_folio(). */
#define FGP_ACCESSED	0x00000001
#define FGP_LOCK	0x00000002
#define FGP_CREAT	0x00000004
#define FGP_WRITE	0x00000008
#define FGP_NOFS	0x00000010
#define FGP_NOWAIT	0x00000020
#define FGP_FOR_MMAP	0x00000040
#define FGP_STABLE	0x00000080
typedef unsigned int fgf_t;

struct file;

/* Mapping lifecycle. */
void address_space_init(struct address_space *mapping, struct inode *host,
			const struct address_space_operations *a_ops);
/* Drop every folio (dirty ones are written first unless discard). */
void address_space_destroy(struct address_space *mapping, bool discard);

/* Lookup / create. */
struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index,
				  fgf_t fgp_flags, gfp_t gfp);
struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index);
struct folio *filemap_lock_folio(struct address_space *mapping, pgoff_t index);
struct folio *read_mapping_folio(struct address_space *mapping, pgoff_t index,
				 struct file *file);
/* Legacy page helpers used by a few core call sites; same object. */
static inline struct page *read_mapping_page(struct address_space *mapping,
					     pgoff_t index, struct file *file)
{
	return (struct page *)read_mapping_folio(mapping, index, file);
}
struct page *grab_cache_page_nowait(struct address_space *mapping, pgoff_t index);

/* Reference counting. */
void folio_get(struct folio *folio);
void folio_put(struct folio *folio);
static inline void put_page(struct page *page) { folio_put(page_folio(page)); }
static inline void get_page(struct page *page) { folio_get(page_folio(page)); }

/* Locking. */
void folio_lock(struct folio *folio);
int  folio_trylock(struct folio *folio);
void folio_unlock(struct folio *folio);
void folio_wait_locked(struct folio *folio);
static inline void lock_page(struct page *page) { folio_lock(page_folio(page)); }
static inline void unlock_page(struct page *page) { folio_unlock(page_folio(page)); }

/* State. */
static inline bool folio_test_locked(const struct folio *f) { return test_bit(PG_locked, &f->flags); }
static inline bool folio_test_uptodate(const struct folio *f) { return test_bit(PG_uptodate, &f->flags); }
static inline bool folio_test_dirty(const struct folio *f) { return test_bit(PG_dirty, &f->flags); }
static inline bool folio_test_writeback(const struct folio *f) { return test_bit(PG_writeback, &f->flags); }
static inline bool folio_test_error(const struct folio *f) { return test_bit(PG_error, &f->flags); }
void folio_mark_uptodate(struct folio *folio);
void folio_clear_uptodate(struct folio *folio);
bool folio_mark_dirty(struct folio *folio);
void folio_clear_dirty(struct folio *folio);
bool folio_clear_dirty_for_io(struct folio *folio);
void folio_set_error(struct folio *folio);
void folio_start_writeback(struct folio *folio);
void folio_end_writeback(struct folio *folio);
void folio_wait_writeback(struct folio *folio);
void folio_redirty_for_writepage(void *wbc, struct folio *folio);
#define PageUptodate(page) folio_test_uptodate(page_folio(page))
#define PageDirty(page) folio_test_dirty(page_folio(page))
#define SetPageUptodate(page) folio_mark_uptodate(page_folio(page))
#define set_page_dirty(page) folio_mark_dirty(page_folio(page))

/* Geometry. */
static inline size_t folio_size(const struct folio *f) { (void)f; return PAGE_SIZE; }
static inline unsigned int folio_shift(const struct folio *f) { (void)f; return PAGE_SHIFT; }
static inline unsigned long folio_nr_pages(const struct folio *f) { (void)f; return 1; }
static inline loff_t folio_pos(const struct folio *f) { return (loff_t)f->index << PAGE_SHIFT; }
static inline pgoff_t folio_index(const struct folio *f) { return f->index; }
static inline size_t offset_in_folio(const struct folio *f, loff_t pos) { (void)f; return pos & ~PAGE_MASK; }
#define offset_in_page(p) ((unsigned long)(p) & ~PAGE_MASK)
static inline void *folio_address(const struct folio *f) { return f->data; }
static inline void *page_address(const struct page *p) { return page_folio(p)->data; }
static inline void *folio_get_private(const struct folio *f) { return f->private; }
static inline void folio_attach_private(struct folio *f, void *p) { f->private = p; set_bit(PG_private, &f->flags); }
static inline void *folio_detach_private(struct folio *f) { void *p = f->private; f->private = NULL; clear_bit(PG_private, &f->flags); return p; }
static inline gfp_t mapping_gfp_mask(const struct address_space *m) { return m->gfp_mask; }
static inline gfp_t mapping_gfp_constraint(const struct address_space *m, gfp_t g) { return m->gfp_mask & g; }
static inline void mapping_set_gfp_mask(struct address_space *m, gfp_t g) { m->gfp_mask = g; }
static inline void mapping_set_error(struct address_space *m, int error) { if (error && !m->wb_err) m->wb_err = error; }
static inline unsigned long mapping_nrpages(const struct address_space *m) { return m->nrpages; }

/* Contents. */
void folio_zero_segment(struct folio *folio, size_t start, size_t end);
void folio_zero_segments(struct folio *folio, size_t start1, size_t end1,
			 size_t start2, size_t end2);
void folio_zero_range(struct folio *folio, size_t start, size_t length);
void folio_fill_tail(struct folio *folio, size_t offset, const char *from, size_t len);
size_t memcpy_from_folio(char *to, struct folio *folio, size_t offset, size_t len);
size_t memcpy_to_folio(struct folio *folio, size_t offset, const char *from, size_t len);
static inline void zero_user_segment(struct page *p, unsigned start, unsigned end) { folio_zero_segment(page_folio(p), start, end); }
static inline void zero_user_segments(struct page *p, unsigned s1, unsigned e1, unsigned s2, unsigned e2) { folio_zero_segments(page_folio(p), s1, e1, s2, e2); }
/* Standalone (non-cached) pages for scratch buffers; platform/src/mem.c. */
struct page *alloc_page(gfp_t gfp);
void __free_page(struct page *page);
static inline void __free_pages(struct page *page, unsigned int order) { (void)order; __free_page(page); }
static inline void clear_page(void *addr) { __builtin_memset(addr, 0, PAGE_SIZE); }
static inline void flush_dcache_page(struct page *p) { (void)p; }
static inline void flush_dcache_folio(struct folio *f) { (void)f; }

/* Write-back and invalidation. */
int filemap_write_and_wait_range(struct address_space *mapping, loff_t lstart, loff_t lend);
int filemap_write_and_wait(struct address_space *mapping);
int filemap_fdatawrite_range(struct address_space *mapping, loff_t start, loff_t end);
int filemap_fdatawait_range(struct address_space *mapping, loff_t start, loff_t end);
int filemap_flush(struct address_space *mapping);
void truncate_inode_pages(struct address_space *mapping, loff_t lstart);
void truncate_inode_pages_range(struct address_space *mapping, loff_t lstart, loff_t lend);
void truncate_inode_pages_final(struct address_space *mapping);
void truncate_pagecache(struct inode *inode, loff_t newsize);
void truncate_setsize(struct inode *inode, loff_t newsize);
unsigned long invalidate_mapping_pages(struct address_space *mapping, pgoff_t start, pgoff_t end);
int invalidate_inode_pages2_range(struct address_space *mapping, pgoff_t start, pgoff_t end);
static inline void filemap_invalidate_lock(struct address_space *m) { down_write(&m->invalidate_lock); }
static inline void filemap_invalidate_unlock(struct address_space *m) { up_write(&m->invalidate_lock); }
static inline void filemap_invalidate_lock_shared(struct address_space *m) { down_read(&m->invalidate_lock); }
static inline void filemap_invalidate_unlock_shared(struct address_space *m) { up_read(&m->invalidate_lock); }

/* Read-ahead is a no-op: metadata access patterns are handled by the core,
 * and file data does not go through this cache. */
struct file_ra_state { unsigned int ra_pages; unsigned int dummy; };
static inline void page_cache_sync_readahead(struct address_space *m, struct file_ra_state *ra,
					     struct file *f, pgoff_t index, unsigned long req_count)
{ (void)m; (void)ra; (void)f; (void)index; (void)req_count; }
static inline void file_ra_state_init(struct file_ra_state *ra, struct address_space *m) { (void)ra; (void)m; }

/* Background writer thread. One per process; started on first mapping. */
int  pagecache_writeback_start(void);
void pagecache_writeback_stop(void);
/* Flush every dirty folio of every mapping belonging to @sb (NULL = all).
 * Ordering (platform review, see docs/progress/platform-review.md): mappings
 * whose host inode is not inode 0 are flushed first, in creation order; the
 * mappings of inode 0 ($MFT/$DATA and its attribute inodes such as
 * $MFT/$BITMAP) go last, newest first, so bitmaps and index blocks reach the
 * device before the MFT records that reference them. */
struct super_block;
int  pagecache_sync_sb(struct super_block *sb);

/* additive (platform review): a folio that belongs to no mapping, laid out
 * and accounted exactly like a cache folio; released with folio_put().
 * alloc_page() (platform/src/mem.c) is built on it. */
struct folio *folio_alloc_standalone(gfp_t gfp);
/* additive (platform review): number of live folios (cached + standalone),
 * and a runtime override of NTFS_PAGECACHE_MAX_FOLIOS for tests (0 = the
 * compile-time default). */
long pagecache_nr_folios(void);
void pagecache_set_max_folios(unsigned long max);

#endif /* _LINUX_PAGEMAP_H */
