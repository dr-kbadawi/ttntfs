/* SPDX-License-Identifier: GPL-2.0 */
/*
 * struct ntfs_bdev over FSBlockDeviceResource.
 *
 * Uses the resource's direct read/write path, not the kernel buffer cache
 * (metadataRead/Write): the core keeps its own metadata cache and double
 * caching would only cost memory. All offsets/lengths the core issues are
 * multiples of the logical block size, which is what the resource requires.
 */
#import "ntfs_bdev_fskit.h"
#include "ntfsport/bdev.h"
#import <os/log.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

static os_log_t bdev_log(void)
{
	static os_log_t log;
	static dispatch_once_t once;
	dispatch_once(&once, ^{ log = os_log_create("org.ntfsmac.NTFS", "bdev"); });
	return log;
}

static int errno_from_nserror(NSError *err, int fallback)
{
	if (!err)
		return fallback;
	if ([err.domain isEqualToString:NSPOSIXErrorDomain] && err.code > 0)
		return (int)err.code;
	return fallback;
}

static ssize_t fskit_pread(struct ntfs_bdev *dev, void *buf, size_t count, u64 offset)
{
	FSBlockDeviceResource *res = (__bridge FSBlockDeviceResource *)dev->priv;
	size_t done = 0;

	if (offset + count > dev->size_bytes)
		return -EINVAL;
	while (done < count) {
		NSError *err = nil;
		size_t n = [res readInto:(char *)buf + done
			      startingAt:(off_t)(offset + done)
				  length:count - done
				   error:&err];
		if (err) {
			os_log_error(bdev_log(), "read @%llu len %zu: %{public}@", offset + done, count - done, err);
			return -errno_from_nserror(err, EIO);
		}
		if (n == 0)
			return -EIO;	/* no progress: treat as error rather than spin */
		done += n;
	}
	return (ssize_t)done;
}

static ssize_t fskit_pwrite(struct ntfs_bdev *dev, const void *buf, size_t count, u64 offset)
{
	FSBlockDeviceResource *res = (__bridge FSBlockDeviceResource *)dev->priv;
	size_t done = 0;

	if (dev->read_only)
		return -EROFS;
	if (offset + count > dev->size_bytes)
		return -EINVAL;
	while (done < count) {
		NSError *err = nil;
		size_t n = [res writeFrom:(void *)((const char *)buf + done)
			       startingAt:(off_t)(offset + done)
				   length:count - done
				    error:&err];
		if (err) {
			os_log_error(bdev_log(), "write @%llu len %zu: %{public}@", offset + done, count - done, err);
			return -errno_from_nserror(err, EIO);
		}
		if (n == 0)
			return -EIO;
		done += n;
	}
	return (ssize_t)done;
}

static int fskit_flush(struct ntfs_bdev *dev)
{
	FSBlockDeviceResource *res = (__bridge FSBlockDeviceResource *)dev->priv;
	NSError *err = nil;

	/* Direct writes are not staged in the buffer cache, but this is the
	 * only flush primitive the resource exposes; it also covers any
	 * metadata writes made through the cache. */
	if (![res metadataFlushWithError:&err]) {
		os_log_error(bdev_log(), "flush: %{public}@", err);
		return -errno_from_nserror(err, EIO);
	}
	return 0;
}

static void fskit_close(struct ntfs_bdev *dev)
{
	if (dev->priv) {
		CFRelease(dev->priv);	/* balances __bridge_retained in create */
		dev->priv = NULL;
	}
	free(dev);
}

static const struct ntfs_bdev_ops fskit_ops = {
	.pread = fskit_pread,
	.pwrite = fskit_pwrite,
	.flush = fskit_flush,
	.discard = NULL,
	.close = fskit_close,
};

struct ntfs_bdev *ntfs_bdev_fskit_create(FSBlockDeviceResource *resource)
{
	struct ntfs_bdev *dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;
	dev->ops = &fskit_ops;
	dev->priv = (void *)CFBridgingRetain(resource);
	dev->logical_block_size = (u32)resource.blockSize;
	dev->physical_block_size = (u32)resource.physicalBlockSize;
	dev->size_bytes = resource.blockSize * resource.blockCount;
	dev->discard_granularity = 0;
	dev->read_only = !resource.isWritable;
	strlcpy(dev->name, resource.BSDName.UTF8String ?: "?", sizeof(dev->name));
	os_log_info(bdev_log(), "open %{public}s: %llu bytes, lbs %u pbs %u%s",
		    dev->name, dev->size_bytes, dev->logical_block_size,
		    dev->physical_block_size, dev->read_only ? " (ro)" : "");
	return dev;
}

void ntfs_bdev_fskit_close(struct ntfs_bdev *dev)
{
	if (dev)
		dev->ops->close(dev);
}

FSBlockDeviceResource *ntfs_bdev_fskit_resource(struct ntfs_bdev *dev)
{
	if (!dev || dev->ops != &fskit_ops)
		return nil;
	return (__bridge FSBlockDeviceResource *)dev->priv;
}
