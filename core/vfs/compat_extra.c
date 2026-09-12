// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * compat_extra.c - the two kernel helpers the compat layer leaves to the
 * vfs stream: dir_emit_dotdot() (needs the parent inode number, an NTFS
 * question) and writeback_iter() (used by mft.c's ntfs_mft_writepages,
 * which the port keeps for its symbol only; $MFT write-back goes through
 * ntfs_mft_aops.write_folio).
 */

#include <linux/fs.h>
#include <linux/writeback.h>
#include <linux/pagemap.h>
#include <linux/bio.h>

#include "ntfs.h"
#include "vfs.h"

bool dir_emit_dotdot(struct file *file, struct dir_context *ctx)
{
	u64 parent = ntfs_vfs_parent_ino(file->f_inode);

	if (parent == (u64)-1)
		parent = file->f_inode->i_ino;
	return ctx->actor(ctx, "..", 2, ctx->pos, parent, DT_DIR);
}

/*
 * writeback_iter - iterate the dirty folios of @mapping in wbc's range
 *
 * Kernel semantics: pass NULL first, then the previous folio; each folio is
 * returned locked with its dirty bit cleared for I/O and writeback started;
 * the caller writes it, then calls folio_end_writeback() and folio_unlock().
 * Returns NULL when done. The dirty list of the mapping is the only index
 * we have, so pick the lowest-indexed dirty folio above the last one each
 * time.
 */
struct folio *writeback_iter(struct address_space *mapping,
			     struct writeback_control *wbc,
			     struct folio *folio, int *error)
{
	pgoff_t next = 0, last = (pgoff_t)-1;

	if (wbc->range_start > 0)
		next = wbc->range_start >> PAGE_SHIFT;
	if (wbc->range_end >= 0)
		last = wbc->range_end >> PAGE_SHIFT;
	if (folio) {
		next = folio->index + 1;
		if (*error) {
			mapping_set_error(mapping, *error);
			wbc->err = *error;
		}
		folio_put(folio);
		if (wbc->nr_to_write > 0)
			wbc->nr_to_write--;
		if (!wbc->nr_to_write && wbc->sync_mode == WB_SYNC_NONE)
			return NULL;
	}
	for (;;) {
		struct folio *best = NULL, *f;

		spin_lock(&mapping->tree_lock);
		list_for_each_entry(f, &mapping->dirty, dirty_list) {
			if (f->index < next || f->index > last)
				continue;
			if (!best || f->index < best->index)
				best = f;
		}
		if (best)
			folio_get(best);
		spin_unlock(&mapping->tree_lock);
		if (!best)
			return NULL;
		folio_lock(best);
		if (best->mapping != mapping || !folio_test_dirty(best)) {
			folio_unlock(best);
			folio_put(best);
			next = best->index + 1;
			continue;
		}
		folio_clear_dirty_for_io(best);
		folio_start_writeback(best);
		return best;
	}
}
