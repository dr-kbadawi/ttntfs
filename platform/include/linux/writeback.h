/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WRITEBACK_H
#define _LINUX_WRITEBACK_H
#include <linux/types.h>
#include <linux/pagemap.h>

enum writeback_sync_modes { WB_SYNC_NONE = 0, WB_SYNC_ALL = 1 };
struct writeback_control {
	long nr_to_write;
	long pages_skipped;
	loff_t range_start;
	loff_t range_end;
	enum writeback_sync_modes sync_mode;
	unsigned for_kupdate:1, for_background:1, tagged_writepages:1, for_reclaim:1, range_cyclic:1, for_sync:1;
	int err;
};
/* Iterate the dirty folios of @mapping within wbc->range_*. Returns each
 * folio locked with writeback started; caller writes it and calls
 * folio_end_writeback()/folio_unlock(). Pass the previous folio back to
 * advance (kernel semantics). Implemented by the pagecache stream. */
struct folio *writeback_iter(struct address_space *mapping, struct writeback_control *wbc,
			     struct folio *folio, int *error);
static inline void balance_dirty_pages_ratelimited(struct address_space *m) { (void)m; }
static inline void balance_dirty_pages_ratelimited_flags(struct address_space *m, unsigned int f) { (void)m; (void)f; }
#endif
