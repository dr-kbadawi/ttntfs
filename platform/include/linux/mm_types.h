/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Page cache data structures for the user-space port.
 *
 * CONTRACT HEADER. A "folio" here is one slot of our own metadata cache:
 * PAGE_SIZE bytes of page-aligned memory, identified by (mapping, index).
 * It is not an MMU page. See docs/PORTING.md §3, rules 2 and 3.
 *
 * Owner of the implementation: platform/pagecache/ (see docs/CONTRACTS.md).
 */
#ifndef _LINUX_MM_TYPES_H
#define _LINUX_MM_TYPES_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>

struct inode;
struct address_space;
struct folio;

/* folio->flags bits */
enum folio_flag_bits {
	PG_locked,
	PG_uptodate,
	PG_dirty,
	PG_writeback,
	PG_error,
	PG_lru,		/* on the mapping's reclaimable list */
	PG_private,	/* folio->private in use */
};

struct folio {
	unsigned long flags;
	atomic_t _refcount;
	struct address_space *mapping;
	pgoff_t index;
	void *data;			/* PAGE_SIZE bytes, PAGE_SIZE aligned */
	void *private;
	struct mutex lock;		/* folio_lock()/folio_unlock() */
	struct list_head lru;		/* mapping->lru list */
	struct list_head dirty_list;	/* mapping->dirty list */
	u64 dirtied_at_ns;		/* CLOCK_MONOTONIC when first dirtied */
	/* Implementation-private storage for the index structure. */
	void *node;
};

/* `struct page` is the folio viewed through its leading fields (the kernel
 * has the same prefix relationship). page_folio()/folio_page() are casts.
 * Only compress.c still touches pages directly. */
struct page {
	unsigned long flags;
	atomic_t _refcount;
	struct address_space *mapping;
	pgoff_t __folio_index;
};
_Static_assert(__builtin_offsetof(struct page, __folio_index) == __builtin_offsetof(struct folio, index), "page/folio prefix");
_Static_assert(__builtin_offsetof(struct page, mapping) == __builtin_offsetof(struct folio, mapping), "page/folio prefix");
#define folio_page(folio, n) ((struct page *)(folio))
#define page_folio(page)     ((struct folio *)(page))

/*
 * Per-mapping backend. Implemented by the port's core/vfs layer (the
 * replacement for Linux fs/ntfs/aops.c). Both callbacks run with the folio
 * locked. read_folio must fill folio->data completely (zero-fill beyond
 * EOF) and call folio_mark_uptodate() on success. write_folio must write
 * folio->data (or the part inside the inode's size) to the device. Errors
 * are negative errno.
 */
struct address_space_operations {
	int (*read_folio)(struct address_space *mapping, struct folio *folio);
	int (*write_folio)(struct address_space *mapping, struct folio *folio);
};

struct address_space {
	struct inode *host;
	const struct address_space_operations *a_ops;
	gfp_t gfp_mask;
	unsigned long flags;
	struct rw_semaphore invalidate_lock;
	struct list_head private_list;

	/* Cache state. Guarded by tree_lock. */
	spinlock_t tree_lock;
	unsigned long nrpages;
	unsigned long nrdirty;
	struct list_head lru;		/* all folios, LRU order */
	struct list_head dirty;		/* dirty folios, oldest first */
	void *tree;			/* implementation-private index */
	int wb_err;			/* sticky first writeback error */
};

#endif /* _LINUX_MM_TYPES_H */
