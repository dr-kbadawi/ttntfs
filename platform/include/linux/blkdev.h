/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BLKDEV_H
#define _LINUX_BLKDEV_H
#include <linux/types.h>
#include <linux/fs.h>
#include <ntfsport/bdev.h>

/* struct block_device is the port's ntfs_bdev. */
#define block_device ntfs_bdev
#define SECTOR_SHIFT 9
#define SECTOR_SIZE 512
#define BLK_DEF_MAX_SECTORS 2560

static inline int blkdev_issue_flush(struct ntfs_bdev *bdev) { return ntfs_bdev_flush(bdev); }
static inline int blkdev_issue_discard(struct ntfs_bdev *bdev, sector_t sector, sector_t nr_sects, gfp_t gfp)
{ (void)gfp; return ntfs_bdev_discard(bdev, (u64)sector << SECTOR_SHIFT, (u64)nr_sects << SECTOR_SHIFT); }
static inline int sync_blockdev(struct ntfs_bdev *bdev) { return ntfs_bdev_flush(bdev); }
static inline int bdev_freeze(struct ntfs_bdev *bdev) { (void)bdev; return 0; }
static inline int bdev_thaw(struct ntfs_bdev *bdev) { (void)bdev; return 0; }
static inline bool bdev_read_only(const struct ntfs_bdev *bdev) { return bdev->read_only; }
#define bdev_nr_sectors(b) (bdev_nr_bytes(b) >> SECTOR_SHIFT)
#endif
