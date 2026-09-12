/* SPDX-License-Identifier: GPL-2.0 */
/* iomap does not exist in the port: file data bypasses the page cache and
 * metadata goes through a_ops. Only the bits Tier 1 touches are here. */
#ifndef _LINUX_IOMAP_H
#define _LINUX_IOMAP_H
#include <linux/types.h>
#include <linux/pagemap.h>
struct iomap_ops;
struct iomap_writeback_ops;
struct iomap_write_ops;
static inline bool iomap_dirty_folio(struct address_space *mapping, struct folio *folio)
{ (void)mapping; return folio_mark_dirty(folio); }
#endif
