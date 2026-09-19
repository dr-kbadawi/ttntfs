// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 TechTag GmbH
/*
 * aops.c - page cache backend for the user-space port.
 *
 * PORT: replaces fs/ntfs/aops.c and the iomap callbacks in fs/ntfs/iomap.c.
 * The port's page cache (platform/pagecache) asks for whole PAGE_SIZE folios
 * through address_space_operations.read_folio / write_folio, both called
 * with the folio locked. Every ntfs inode has one of two tables:
 *
 *   ntfs_aops      resident attributes are copied from/to the mft record
 *                  (ntfs_read_iomap_begin_resident / write_iomap_end_resident
 *                  upstream); non-resident attributes go VCN -> LCN through
 *                  the run list to the device; compressed data is decoded by
 *                  compress.c; holes and the area beyond initialized_size
 *                  read as zero.
 *   ntfs_mft_aops  same reads; writes go through mft.c's ntfs_write_mft_block
 *                  which applies the MST fixups and skips records whose
 *                  inodes are dirty in memory (ntfs_mft_writepages upstream).
 *
 * This file also provides the direct device I/O helpers the read/write fast
 * path in api.c uses (docs/PORTING.md §3, rule 3) and the zero-range helper
 * that stands in for iomap_zero_range().
 *
 * Locking: a folio lock nests outside the inode's run list lock and outside
 * the mft folio locks taken by map_mft_record(); it must never wait on an
 * mrec_lock (see ntfs_write_folio_resident).
 */

#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/bio.h>

#include "attrib.h"
#include "mft.h"
#include "ntfs.h"
#include "debug.h"
#include "vfs.h"

/* ---- run list mapping ------------------------------------------------- */

/*
 * ntfs_vfs_map_vcn - map @vcn to an lcn and the run length from there
 * @ni:		inode whose attribute to map
 * @vcn:	vcn to map
 * @max:	upper bound on *@count
 * @count:	on return, clusters from @vcn to the end of the run (<= @max)
 *
 * Maps the run list on demand. Returns the lcn (>= 0), LCN_HOLE / LCN_DELALLOC
 * for unallocated space, LCN_ENOENT beyond the end of the attribute, or a
 * negative LCN_E* code on error.
 */
static s64 ntfs_vfs_map_vcn(struct ntfs_inode *ni, s64 vcn, s64 max, s64 *count)
{
	struct runlist_element *rl;
	s64 lcn;

	*count = 1;
	down_read(&ni->runlist.lock);
	lcn = ntfs_attr_vcn_to_lcn_nolock(ni, vcn, false);
	if (lcn >= LCN_HOLE) {
		rl = __ntfs_attr_find_vcn_nolock(&ni->runlist, vcn);
		if (!IS_ERR(rl) && rl->length > vcn - rl->vcn)
			*count = min(max, rl->length - (vcn - rl->vcn));
	}
	up_read(&ni->runlist.lock);
	return lcn;
}

static int ntfs_vfs_lcn_error(struct ntfs_inode *ni, s64 lcn)
{
	switch ((int)lcn) {
	case LCN_ENOMEM:
		return -ENOMEM;
	default:
		ntfs_error(ni->vol->sb,
			   "Failed to map vcn of inode 0x%llx (lcn %lld). Run chkdsk.",
			   ni->mft_no, (long long)lcn);
		return -EIO;
	}
}

/* Device write through a bio so the raw device mapping (used by
 * compress.c) is invalidated by the bio layer. */
static int ntfs_vfs_dev_write(struct ntfs_volume *vol, const void *buf,
			      u64 dev_off, size_t len)
{
	struct bio *bio;
	int err;

	bio = bio_alloc(vol->sb->s_bdev, 1, REQ_OP_WRITE, GFP_NOIO);
	if (!bio)
		return -ENOMEM;
	bio->bi_iter.bi_sector = dev_off >> SECTOR_SHIFT;
	if (!bio_add_vmalloc_chunk(bio, (void *)buf, (unsigned int)len)) {
		bio_put(bio);
		return -EIO;
	}
	err = submit_bio_wait(bio);
	bio_put(bio);
	return err;
}

/*
 * Walk the runs of [pos, pos + len) of a non-resident attribute and hand
 * each contiguous device extent (or hole) to @fn. @pos and @len are sector
 * aligned. Holes are reported with lcn == LCN_HOLE; a vcn beyond the
 * attribute's allocation with lcn == LCN_ENOENT.
 */
typedef int (*ntfs_vfs_extent_fn)(struct ntfs_inode *ni, s64 lcn, u64 dev_off,
				  loff_t pos, size_t len, void *arg);

static int ntfs_vfs_walk_runs(struct ntfs_inode *ni, loff_t pos, size_t len,
			      ntfs_vfs_extent_fn fn, void *arg)
{
	struct ntfs_volume *vol = ni->vol;
	loff_t end = pos + len;
	int err;

	while (pos < end) {
		s64 vcn = ntfs_bytes_to_cluster(vol, pos);
		unsigned int vcn_ofs = ntfs_bytes_to_cluster_off(vol, pos);
		s64 lcn, count;
		size_t chunk;

		lcn = ntfs_vfs_map_vcn(ni, vcn, ntfs_bytes_to_cluster(vol,
					round_up(end, vol->cluster_size)) - vcn,
				       &count);
		if (lcn < LCN_HOLE && lcn != LCN_ENOENT)
			return ntfs_vfs_lcn_error(ni, lcn);
		if (lcn == LCN_ENOENT) {
			/* Beyond the allocation: one report for the rest. */
			return fn(ni, LCN_ENOENT, 0, pos, end - pos, arg);
		}
		chunk = min_t(size_t, ntfs_cluster_to_bytes(vol, count) - vcn_ofs,
			      (size_t)(end - pos));
		if (lcn >= 0)
			err = fn(ni, lcn, ntfs_cluster_to_bytes(vol, lcn) + vcn_ofs,
				 pos, chunk, arg);
		else
			err = fn(ni, LCN_HOLE, 0, pos, chunk, arg);
		if (err)
			return err;
		pos += chunk;
	}
	return 0;
}

/* ---- read ------------------------------------------------------------- */

struct ntfs_vfs_buf {
	u8 *base;		/* buffer covering [start, start + len) */
	loff_t start;
};

static int ntfs_vfs_read_extent(struct ntfs_inode *ni, s64 lcn, u64 dev_off,
				loff_t pos, size_t len, void *arg)
{
	struct ntfs_vfs_buf *b = arg;
	u8 *dst = b->base + (pos - b->start);

	if (lcn < 0) {
		memset(dst, 0, len);
		return 0;
	}
	return ntfs_bdev_read(ni->vol->sb->s_bdev, dst, dev_off, len);
}

/*
 * ntfs_vfs_direct_read - read file data straight from the device
 *
 * @pos and @len must be multiples of the device's logical block size. The
 * caller has clipped the range to initialized_size (bytes beyond it are
 * zero by definition) and has flushed cached pages for the range.
 */
int ntfs_vfs_direct_read(struct inode *vi, void *buf, loff_t pos, size_t len)
{
	struct ntfs_vfs_buf b = { .base = buf, .start = pos };

	return ntfs_vfs_walk_runs(NTFS_I(vi), pos, len, ntfs_vfs_read_extent, &b);
}

static int ntfs_read_folio_resident(struct inode *vi, struct folio *folio)
{
	struct ntfs_inode *base_ni, *ni = NTFS_I(vi);
	struct ntfs_attr_search_ctx *ctx;
	loff_t pos = folio_pos(folio), i_size;
	u32 attr_len;
	int err;

	if (NInoAttr(ni))
		base_ni = ni->ext.base_ntfs_ino;
	else
		base_ni = ni;

	ctx = ntfs_attr_get_search_ctx(base_ni, NULL);
	if (!ctx)
		return -ENOMEM;
	err = ntfs_attr_lookup(ni->type, ni->name, ni->name_len,
			CASE_SENSITIVE, 0, NULL, 0, ctx);
	if (unlikely(err)) {
		if (err == -ENOENT)
			err = -EIO;
		goto out;
	}
	if (unlikely(ctx->attr->non_resident)) {
		/* Raced with a conversion; the caller retries through the
		 * non-resident path on the next read. */
		err = -EAGAIN;
		goto out;
	}

	attr_len = le32_to_cpu(ctx->attr->data.resident.value_length);
	if (unlikely(attr_len > ni->initialized_size))
		attr_len = ni->initialized_size;
	i_size = i_size_read(vi);
	if (unlikely(attr_len > i_size))
		attr_len = i_size;	/* Race with shrinking truncate. */

	if (pos < attr_len) {
		size_t n = min_t(size_t, attr_len - pos, PAGE_SIZE);
		u8 *kattr = (u8 *)ctx->attr +
			le16_to_cpu(ctx->attr->data.resident.value_offset);

		memcpy(folio->data, kattr + pos, n);
		if (n < PAGE_SIZE)
			folio_zero_segment(folio, n, PAGE_SIZE);
	} else {
		folio_zero_segment(folio, 0, PAGE_SIZE);
	}
	folio_mark_uptodate(folio);
out:
	ntfs_attr_put_search_ctx(ctx);
	return err;
}

static int ntfs_read_folio_non_resident(struct inode *vi, struct folio *folio)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	loff_t pos = folio_pos(folio), init_size, i_size;
	unsigned long flags;
	struct ntfs_vfs_buf b = { .base = folio->data, .start = pos };
	size_t len;
	int err;

	read_lock_irqsave(&ni->size_lock, flags);
	init_size = ni->initialized_size;
	i_size = i_size_read(vi);
	read_unlock_irqrestore(&ni->size_lock, flags);
	if (init_size > i_size)
		init_size = i_size;

	if (pos >= init_size) {
		folio_zero_segment(folio, 0, PAGE_SIZE);
		folio_mark_uptodate(folio);
		return 0;
	}
	/* Read whole device blocks; the tail beyond initialized_size is
	 * zeroed below (it is inside the allocation or reported as
	 * LCN_ENOENT and zero-filled by the walker). */
	len = min_t(size_t, PAGE_SIZE,
		    round_up(init_size - pos, 1UL << vi->i_blkbits));
	err = ntfs_vfs_walk_runs(ni, pos, len, ntfs_vfs_read_extent, &b);
	if (err)
		return err;
	if (init_size - pos < PAGE_SIZE)
		folio_zero_segment(folio, init_size - pos, PAGE_SIZE);
	folio_mark_uptodate(folio);
	return 0;
}

/*
 * ntfs_read_folio - fill a page cache folio
 *
 * Mirrors upstream ntfs_read_folio(): encrypted data is refused, compressed
 * unnamed $DATA is decoded by compress.c (which, as in the kernel, unlocks
 * the folio itself: re-take the lock so the caller's unlock balances).
 */
static int ntfs_read_folio(struct address_space *mapping, struct folio *folio)
{
	struct inode *vi = mapping->host;
	struct ntfs_inode *ni = NTFS_I(vi);
	int err;

	if (ni->type != AT_INDEX_ALLOCATION) {
		if (NInoEncrypted(ni))
			return -EOPNOTSUPP;
		if (NInoNonResident(ni) && NInoCompressed(ni)) {
			err = ntfs_read_compressed_block(folio);
			folio_lock(folio);
			return err;
		}
	}
	if (!NInoNonResident(ni)) {
		if (ni->type == AT_INDEX_ALLOCATION || ni->type == AT_INDEX_ROOT) {
			/* Small index without an allocation: nothing here. */
			folio_zero_segment(folio, 0, PAGE_SIZE);
			folio_mark_uptodate(folio);
			return 0;
		}
		return ntfs_read_folio_resident(vi, folio);
	}
	return ntfs_read_folio_non_resident(vi, folio);
}

/* ---- write ------------------------------------------------------------ */

static int ntfs_vfs_write_extent(struct ntfs_inode *ni, s64 lcn, u64 dev_off,
				 loff_t pos, size_t len, void *arg)
{
	struct ntfs_vfs_buf *b = arg;
	const u8 *src = b->base + (pos - b->start);

	if (lcn == LCN_ENOENT) {
		ntfs_error(ni->vol->sb,
			   "Write beyond allocation of inode 0x%llx (pos 0x%llx).",
			   ni->mft_no, (unsigned long long)pos);
		return -EIO;
	}
	if (lcn < 0) {
		/*
		 * A hole under dirty data. The write paths allocate before
		 * they dirty a folio, so the only way here is a dirty page
		 * whose tail spans a hole created by an extending truncate;
		 * that tail is zero and needs no I/O.
		 */
		return 0;
	}
	return ntfs_vfs_dev_write(ni->vol, src, dev_off, len);
}

/*
 * ntfs_vfs_direct_write - write file data straight to the device
 *
 * @pos/@len sector aligned; clusters must be allocated (holes are skipped
 * as zero, so the caller must have allocated the range first).
 */
int ntfs_vfs_direct_write(struct inode *vi, const void *buf, loff_t pos, size_t len)
{
	struct ntfs_vfs_buf b = { .base = (u8 *)buf, .start = pos };

	return ntfs_vfs_walk_runs(NTFS_I(vi), pos, len, ntfs_vfs_write_extent, &b);
}

static int ntfs_write_folio_non_resident(struct inode *vi, struct folio *folio)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	loff_t pos = folio_pos(folio), limit;
	unsigned long flags;
	struct ntfs_vfs_buf b = { .base = folio->data, .start = pos };
	size_t len;

	/*
	 * Write everything the allocation covers: the parts of the folio
	 * beyond initialized_size/i_size are zero (or, for metadata, the
	 * valid tail of the attribute) and writing them is harmless, while
	 * clipping to a size that another thread is about to extend would
	 * lose data.
	 */
	read_lock_irqsave(&ni->size_lock, flags);
	limit = ni->allocated_size;
	read_unlock_irqrestore(&ni->size_lock, flags);
	if (pos >= limit)
		return 0;
	len = min_t(size_t, PAGE_SIZE, round_up(limit - pos, 1UL << vi->i_blkbits));
	return ntfs_vfs_walk_runs(ni, pos, len, ntfs_vfs_write_extent, &b);
}

/*
 * Resident attribute: the folio is a view of the value in the mft record
 * (upstream ntfs_write_iomap_end_resident). Needs the mrec_lock; a folio
 * lock must not wait for it (paths hold mrec_lock and then lock folios), so
 * try once and leave the folio dirty for the next pass otherwise.
 */
static int ntfs_write_folio_resident(struct inode *vi, struct folio *folio)
{
	struct ntfs_inode *base_ni, *ni = NTFS_I(vi);
	struct ntfs_attr_search_ctx *ctx;
	loff_t pos = folio_pos(folio);
	u32 attr_len;
	int err = 0;

	if (NInoAttr(ni))
		base_ni = ni->ext.base_ntfs_ino;
	else
		base_ni = ni;

	if (!mutex_trylock(&base_ni->mrec_lock)) {
		folio_redirty_for_writepage(NULL, folio);
		return 0;
	}
	ctx = ntfs_attr_get_search_ctx(base_ni, NULL);
	if (!ctx) {
		err = -ENOMEM;
		goto unlock;
	}
	err = ntfs_attr_lookup(ni->type, ni->name, ni->name_len,
			       CASE_SENSITIVE, 0, NULL, 0, ctx);
	if (err) {
		if (err == -ENOENT)
			err = -EIO;
		goto out;
	}
	if (ctx->attr->non_resident) {
		ntfs_attr_put_search_ctx(ctx);
		mutex_unlock(&base_ni->mrec_lock);
		return ntfs_write_folio_non_resident(vi, folio);
	}
	attr_len = le32_to_cpu(ctx->attr->data.resident.value_length);
	if (pos < attr_len) {
		u8 *kattr = (u8 *)ctx->attr +
			le16_to_cpu(ctx->attr->data.resident.value_offset);

		memcpy(kattr + pos, folio->data, min_t(size_t, attr_len - pos, PAGE_SIZE));
		mark_mft_record_dirty(ctx->ntfs_ino);
	}
out:
	ntfs_attr_put_search_ctx(ctx);
unlock:
	mutex_unlock(&base_ni->mrec_lock);
	return err;
}

static int ntfs_write_folio(struct address_space *mapping, struct folio *folio)
{
	struct inode *vi = mapping->host;
	struct ntfs_inode *ni = NTFS_I(vi);

	if (NVolShutdown(ni->vol))
		return -EIO;
	if (ni->type != AT_INDEX_ALLOCATION) {
		if (NInoEncrypted(ni))
			return -EOPNOTSUPP;
		if (NInoNonResident(ni) && NInoCompressed(ni)) {
			/*
			 * compress.c writes compression blocks itself
			 * (ntfs_compress_write) and leaves the folios clean;
			 * a dirty folio here has nothing to add.
			 */
			return 0;
		}
	}
	if (!NInoNonResident(ni)) {
		if (ni->type == AT_INDEX_ALLOCATION || ni->type == AT_INDEX_ROOT)
			return 0;
		return ntfs_write_folio_resident(vi, folio);
	}
	return ntfs_write_folio_non_resident(vi, folio);
}

/*
 * $MFT: mft.c's ntfs_write_mft_block() (exported for the port) applies the
 * MST fixups, skips records owned by dirty in-memory inodes and syncs the
 * mirror. Like a kernel ->writepage it unlocks the folio and ends writeback
 * itself, so re-take the lock for our caller; folio_end_writeback() twice is
 * harmless.
 */
static int ntfs_mft_write_folio(struct address_space *mapping, struct folio *folio)
{
	struct writeback_control wbc = { .sync_mode = WB_SYNC_ALL };
	int err;

	if (NVolShutdown(NTFS_I(mapping->host)->vol))
		return -EIO;
	err = ntfs_write_mft_block(folio, &wbc);
	folio_lock(folio);
	return err;
}

const struct address_space_operations ntfs_aops = {
	.read_folio	= ntfs_read_folio,
	.write_folio	= ntfs_write_folio,
};

const struct address_space_operations ntfs_mft_aops = {
	.read_folio	= ntfs_read_folio,
	.write_folio	= ntfs_mft_write_folio,
};

/* ---- zeroing ---------------------------------------------------------- */

/*
 * ntfs_dio_zero_range - zero a device byte range
 * @offset/@length: device offsets (as upstream's blkdev_issue_zeroout use)
 */
int ntfs_dio_zero_range(struct inode *vi, loff_t offset, loff_t length)
{
	struct ntfs_volume *vol = NTFS_I(vi)->vol;
	size_t chunk = min_t(size_t, length, 1UL << 20);
	void *zero;
	int err = 0;

	if ((offset | length) & (SECTOR_SIZE - 1))
		return -EINVAL;
	if (!length)
		return 0;
	zero = kzalloc(chunk, GFP_NOFS);
	if (!zero)
		return -ENOMEM;
	while (length > 0 && !err) {
		size_t n = min_t(size_t, length, chunk);

		err = ntfs_vfs_dev_write(vol, zero, offset, n);
		offset += n;
		length -= n;
	}
	kfree(zero);
	return err;
}

static int ntfs_vfs_zero_extent(struct ntfs_inode *ni, s64 lcn, u64 dev_off,
				loff_t pos, size_t len, void *arg)
{
	if (lcn < 0)
		return 0;	/* holes and space beyond the allocation are zero */
	return ntfs_dio_zero_range(VFS_I(ni), dev_off, len);
}

/*
 * ntfs_vfs_zero_range - zero [start, start + len) of a non-resident attribute
 *
 * Stands in for iomap_zero_range(): partial pages go through the page cache
 * (so an in-memory copy stays coherent), whole pages are zeroed on the
 * device and dropped from the cache. Holes are left alone.
 */
int ntfs_vfs_zero_range(struct inode *vi, loff_t start, loff_t len)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct address_space *mapping = vi->i_mapping;
	loff_t pos = start, end = start + len;
	int err = 0;

	if (len <= 0)
		return 0;
	if (!NInoNonResident(ni)) {
		/* Resident: the value is zero-extended by the resize. */
		return 0;
	}
	while (pos < end && !err) {
		unsigned int page_off = pos & ~PAGE_MASK;
		size_t n;

		if (page_off || end - pos < (loff_t)PAGE_SIZE) {
			struct folio *folio;

			n = min_t(size_t, PAGE_SIZE - page_off, end - pos);
			folio = read_mapping_folio(mapping, pos >> PAGE_SHIFT, NULL);
			if (IS_ERR(folio)) {
				err = PTR_ERR(folio);
				break;
			}
			folio_lock(folio);
			folio_zero_range(folio, page_off, n);
			folio_mark_dirty(folio);
			folio_unlock(folio);
			folio_put(folio);
		} else {
			n = round_down(end - pos, PAGE_SIZE);
			err = filemap_write_and_wait_range(mapping, pos, pos + n - 1);
			if (err)
				break;
			invalidate_inode_pages2_range(mapping, pos >> PAGE_SHIFT,
						      (pos + n - 1) >> PAGE_SHIFT);
			err = ntfs_vfs_walk_runs(ni, pos, n, ntfs_vfs_zero_extent, NULL);
		}
		pos += n;
	}
	return err;
}
