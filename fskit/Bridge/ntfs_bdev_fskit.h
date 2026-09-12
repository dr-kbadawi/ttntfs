/* SPDX-License-Identifier: GPL-2.0 */
/*
 * struct ntfs_bdev over an FSKit FSBlockDeviceResource.
 *
 * Deliberately does not include ntfsport/bdev.h: that header pulls in the
 * kernel-compat types (struct rb_node, ...) which collide with Darwin's
 * <sys/rbtree.h> when imported into Swift. Swift only ever sees the
 * device as an opaque pointer.
 */
#ifndef NTFS_BDEV_FSKIT_H
#define NTFS_BDEV_FSKIT_H

#import <FSKit/FSKit.h>
#include <stdbool.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

struct ntfs_bdev;

/* Wraps @resource (retained). Returns NULL on failure. The device's block
 * size, size and writability come from the resource. */
struct ntfs_bdev * _Nullable ntfs_bdev_fskit_create(FSBlockDeviceResource *resource);

/* Releases a device created by ntfs_bdev_fskit_create(). */
void ntfs_bdev_fskit_close(struct ntfs_bdev *dev);

/* The resource a device was created from (unretained). */
FSBlockDeviceResource * _Nullable ntfs_bdev_fskit_resource(struct ntfs_bdev *dev);

/* Force the device read-only (FSKit's --rdonly load option). Cannot make a
 * read-only resource writable: returns the effective state. */
bool ntfs_bdev_fskit_set_read_only(struct ntfs_bdev *dev, bool read_only);
bool ntfs_bdev_fskit_is_read_only(struct ntfs_bdev *dev);

/* Geometry, for logging and the mount status file. */
uint64_t ntfs_bdev_fskit_size_bytes(struct ntfs_bdev *dev);
uint32_t ntfs_bdev_fskit_block_size(struct ntfs_bdev *dev);

NS_ASSUME_NONNULL_END

#endif /* NTFS_BDEV_FSKIT_H */
