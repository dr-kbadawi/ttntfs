// SPDX-License-Identifier: GPL-2.0
/*
 * bdev_mapping.c - the page cache over a raw block device (bdev->bd_mapping)
 *
 * Kernel code reaches for sb->s_bdev->bd_mapping whenever it wants to read or
 * write device bytes through the page cache rather than through a file: the
 * readahead and writeback in ntfs_empty_logfile() are the path that matters
 * here. Nothing in the port dereferences it defensively, so a bdev without one
 * does not degrade, it crashes.
 *
 * This used to live in bdev_file.c and was set up only for file-backed devices,
 * which is why it went unnoticed: the tools and every fixture test open images
 * through that path. The FSKit extension builds its own struct ntfs_bdev over
 * an FSBlockDeviceResource, got NULL, and died in idx_of() the first time a
 * volume with a non-empty journal was mounted read-write -- the one case the
 * fixtures never produced.
 *
 * The folio operations only ever call ntfs_bdev_read/ntfs_bdev_write, which
 * dispatch through dev->ops, so they are correct for any block device. Every
 * bdev implementation should call ntfs_bdev_attach_mapping() when it is
 * constructed and ntfs_bdev_detach_mapping() when it is torn down.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "ntfsport/bdev.h"

/* The mapping has no host inode, so the device is stashed in the otherwise
 * unused private_list head. Both ends point at it so a caller that walks the
 * list from either direction finds the same thing. */
static struct ntfs_bdev *mapping_bdev(struct address_space *m)
{
	return (struct ntfs_bdev *)m->private_list.next;
}

static int bdev_read_folio(struct address_space *mapping, struct folio *folio)
{
	struct ntfs_bdev *dev = mapping_bdev(mapping);
	u64 off = folio_pos(folio);
	size_t len = PAGE_SIZE;
	int err;

	if (off >= dev->size_bytes) {
		memset(folio->data, 0, PAGE_SIZE);
		folio_mark_uptodate(folio);
		return 0;
	}
	if (dev->size_bytes - off < len)
		len = (size_t)(dev->size_bytes - off);
	err = ntfs_bdev_read(dev, folio->data, off, len);
	if (err)
		return err;
	if (len < PAGE_SIZE)
		memset((char *)folio->data + len, 0, PAGE_SIZE - len);
	folio_mark_uptodate(folio);
	return 0;
}

static int bdev_write_folio(struct address_space *mapping, struct folio *folio)
{
	struct ntfs_bdev *dev = mapping_bdev(mapping);
	u64 off = folio_pos(folio);
	size_t len = PAGE_SIZE;

	if (off >= dev->size_bytes)
		return 0;
	if (dev->size_bytes - off < len)
		len = (size_t)(dev->size_bytes - off);
	return ntfs_bdev_write(dev, folio->data, off, len);
}

static const struct address_space_operations bdev_aops = {
	.read_folio = bdev_read_folio,
	.write_folio = bdev_write_folio,
};

int ntfs_bdev_attach_mapping(struct ntfs_bdev *dev)
{
	if (!dev)
		return -EINVAL;
	if (dev->bd_mapping)
		return 0;
	dev->bd_mapping = calloc(1, sizeof(*dev->bd_mapping));
	if (!dev->bd_mapping)
		return -ENOMEM;
	address_space_init(dev->bd_mapping, NULL, &bdev_aops);
	dev->bd_mapping->private_list.next = (struct list_head *)dev;
	dev->bd_mapping->private_list.prev = (struct list_head *)dev;
	return 0;
}

void ntfs_bdev_detach_mapping(struct ntfs_bdev *dev)
{
	if (!dev || !dev->bd_mapping)
		return;
	address_space_destroy(dev->bd_mapping, false);
	free(dev->bd_mapping);
	dev->bd_mapping = NULL;
}
