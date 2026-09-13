/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BLKDEV_H
#define _LINUX_BLKDEV_H
#include <linux/types.h>
#include <linux/fs.h>
#include <ntfsport/bdev.h>
#include <linux/pagemap.h>

/* struct block_device is the port's ntfs_bdev. */
#define block_device ntfs_bdev
#define SECTOR_SHIFT 9
#define SECTOR_SIZE 512
#define BLK_DEF_MAX_SECTORS 2560

/*
 * These two are NOT the same operation, and conflating them is expensive.
 *
 *   blkdev_issue_flush()  issues a device cache flush (a FLUSH request in
 *                         Linux; F_FULLFSYNC or the resource's metadataFlush
 *                         here). This is the durability barrier and it is slow.
 *   sync_blockdev()       writes back the *block device's page cache*
 *                         (filemap_write_and_wait(bdev->bd_mapping)) and issues
 *                         no barrier at all.
 *
 * Both used to map to ntfs_bdev_flush(), because bd_mapping did not exist and
 * the shim was written when "flush the block device" read like one idea. The
 * cost of that: the driver calls sync_blockdev() and blkdev_issue_flush() in
 * sequence -- ntfs_fsync() in core/vfs/file.c does, and so does ntfs_sync_fs()
 * in core/ntfs/super.c -- so every fsync paid two full device flushes, and a
 * volume sync paid three once sync_filesystem() added its own. Measured
 * 2026-09-14: the flush was ~100% of a 21 ms core-only sync, and fsync through
 * FSKit cost 75 ms against Apple's exFAT at 21 ms.
 *
 * bd_mapping exists now (every bdev attaches one), so sync_blockdev() can mean
 * what Linux means by it.
 */
static inline int blkdev_issue_flush(struct ntfs_bdev *bdev) { return ntfs_bdev_flush(bdev); }
static inline int blkdev_issue_discard(struct ntfs_bdev *bdev, sector_t sector, sector_t nr_sects, gfp_t gfp)
{ (void)gfp; return ntfs_bdev_discard(bdev, (u64)sector << SECTOR_SHIFT, (u64)nr_sects << SECTOR_SHIFT); }
static inline int sync_blockdev(struct ntfs_bdev *bdev)
{
	return bdev && bdev->bd_mapping ? filemap_write_and_wait(bdev->bd_mapping) : 0;
}
static inline int bdev_freeze(struct ntfs_bdev *bdev) { (void)bdev; return 0; }
static inline int bdev_thaw(struct ntfs_bdev *bdev) { (void)bdev; return 0; }
static inline bool bdev_read_only(const struct ntfs_bdev *bdev) { return bdev->read_only; }
#define bdev_nr_sectors(b) (bdev_nr_bytes(b) >> SECTOR_SHIFT)
#endif
