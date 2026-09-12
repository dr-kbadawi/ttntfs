/* SPDX-License-Identifier: GPL-2.0 */
/*
 * struct ntfs_bdev over FSBlockDeviceResource.
 *
 * Uses the resource's direct read/write path, not the kernel buffer cache
 * (metadataRead/Write): the core keeps its own metadata cache and double
 * caching would only cost memory. Every request the core issues is handed
 * to the resource as ONE call for the whole byte range (no per-sector
 * loop); the loop below only continues after a genuinely partial transfer,
 * which the resource is not expected to produce.
 *
 * The resource requires offsets and lengths to be multiples of its logical
 * block size (FSBlockDeviceResource.blockSize). The core promises that
 * (bdev.h: page cache issues PAGE_SIZE-aligned I/O, the data path aligns
 * edges); a misaligned request is a core bug and is refused with -EINVAL
 * here rather than passed on to fail obscurely in the kernel.
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
	dispatch_once(&once, ^{ log = os_log_create("ch.techtag.ntfs", "bdev"); });
	return log;
}

static int errno_from_nserror(NSError *err, int fallback)
{
	if (!err)
		return fallback;
	if ([err.domain isEqualToString:NSPOSIXErrorDomain] && err.code > 0 && err.code <= ELAST)
		return (int)err.code;
	return fallback;
}

/* Returns 0 when [offset, offset+count) is inside the device and aligned. */
static int check_range(struct ntfs_bdev *dev, u64 offset, size_t count, const char *what)
{
	u32 lbs = dev->logical_block_size;

	if (count == 0)
		return 0;
	if (offset > dev->size_bytes || count > dev->size_bytes - offset) {
		os_log_error(bdev_log(), "%{public}s @%llu len %zu: beyond device end (%llu)",
			     what, offset, count, dev->size_bytes);
		return -EINVAL;
	}
	if (lbs && ((offset % lbs) || (count % lbs))) {
		os_log_fault(bdev_log(), "%{public}s @%llu len %zu: not a multiple of the block size %u",
			     what, offset, count, lbs);
		return -EINVAL;
	}
	return 0;
}

static ssize_t fskit_pread(struct ntfs_bdev *dev, void *buf, size_t count, u64 offset)
{
	FSBlockDeviceResource *res = (__bridge FSBlockDeviceResource *)dev->priv;
	size_t done = 0;
	int rc = check_range(dev, offset, count, "read");

	if (rc)
		return rc;
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
		if (n == 0) {
			os_log_error(bdev_log(), "read @%llu len %zu: no progress", offset + done, count - done);
			return -EIO;	/* no progress: treat as error rather than spin */
		}
		if (n < count - done)
			os_log_info(bdev_log(), "read @%llu: partial %zu of %zu, continuing", offset + done, n, count - done);
		done += n;
	}
	return (ssize_t)done;
}

static ssize_t fskit_pwrite(struct ntfs_bdev *dev, const void *buf, size_t count, u64 offset)
{
	FSBlockDeviceResource *res = (__bridge FSBlockDeviceResource *)dev->priv;
	size_t done = 0;
	int rc;

	if (dev->read_only)
		return -EROFS;
	rc = check_range(dev, offset, count, "write");
	if (rc)
		return rc;
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
		if (n == 0) {
			os_log_error(bdev_log(), "write @%llu len %zu: no progress", offset + done, count - done);
			return -EIO;
		}
		if (n < count - done)
			os_log_info(bdev_log(), "write @%llu: partial %zu of %zu, continuing", offset + done, n, count - done);
		done += n;
	}
	return (ssize_t)done;
}

static int fskit_flush(struct ntfs_bdev *dev)
{
	FSBlockDeviceResource *res = (__bridge FSBlockDeviceResource *)dev->priv;
	NSError *err = nil;

	if (dev->read_only)
		return 0;
	/* Direct writes are not staged in the FSKit buffer cache, but
	 * metadataFlush is the only flush primitive the resource exposes and
	 * it reaches the device's write cache (it is what Apple's own modules
	 * call for fsync). */
	if (![res metadataFlushWithError:&err]) {
		os_log_error(bdev_log(), "flush: %{public}@", err);
		return -errno_from_nserror(err, EIO);
	}
	return 0;
}

static void fskit_close(struct ntfs_bdev *dev)
{
	if (dev->priv) {
		CFRelease(dev->priv);	/* balances CFBridgingRetain in create */
		dev->priv = NULL;
	}
	free(dev);
}

/* FSBlockDeviceResource has no TRIM/UNMAP primitive on macOS 26; the core
 * sees discard_granularity 0 and never asks. */
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
	if (resource.blockSize == 0 || resource.blockCount == 0) {
		os_log_error(bdev_log(), "resource %{public}@ has no geometry", resource.BSDName);
		free(dev);
		return NULL;
	}
	dev->ops = &fskit_ops;
	dev->priv = (void *)CFBridgingRetain(resource);
	dev->logical_block_size = (u32)resource.blockSize;
	dev->physical_block_size = (u32)(resource.physicalBlockSize ?: resource.blockSize);
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

bool ntfs_bdev_fskit_set_read_only(struct ntfs_bdev *dev, bool read_only)
{
	FSBlockDeviceResource *res = ntfs_bdev_fskit_resource(dev);

	if (!res)
		return true;
	dev->read_only = read_only || !res.isWritable;
	return dev->read_only;
}

bool ntfs_bdev_fskit_is_read_only(struct ntfs_bdev *dev)
{
	return dev ? dev->read_only : true;
}

uint64_t ntfs_bdev_fskit_size_bytes(struct ntfs_bdev *dev)
{
	return dev ? dev->size_bytes : 0;
}

uint32_t ntfs_bdev_fskit_block_size(struct ntfs_bdev *dev)
{
	return dev ? dev->logical_block_size : 0;
}
