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
#include <stdatomic.h>
#include <time.h>

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

/*
 * Diagnostic counters for the read path. Question they answer: is the device
 * busy for the whole of a sequential read, or are there gaps between our
 * requests? If the time inside readInto: accounts for nearly all the wall clock,
 * we are bandwidth-bound and readahead buys nothing; if it does not, we are
 * waiting between requests and pipelining would pay. Logged on close.
 */
static _Atomic uint64_t ra_calls, ra_bytes, ra_ns, ra_first_ns, ra_last_ns;

static uint64_t now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_MONOTONIC);
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
		uint64_t t0 = now_ns();
		size_t n = [res readInto:(char *)buf + done
			      startingAt:(off_t)(offset + done)
				  length:count - done
				   error:&err];
		uint64_t t1 = now_ns();
		{
			uint64_t zero = 0, calls, bytes, busy, span;

			atomic_compare_exchange_strong(&ra_first_ns, &zero, t0);
			atomic_store(&ra_last_ns, t1);
			atomic_fetch_add(&ra_ns, t1 - t0);
			atomic_fetch_add(&ra_bytes, n);
			calls = atomic_fetch_add(&ra_calls, 1) + 1;
			bytes = atomic_load(&ra_bytes);
			/* Report per window rather than at close: the module process
			 * outlives the unmount, so a close-time report never arrives
			 * while anyone is watching. */
			if (bytes >= (192ULL << 20)) {
				busy = atomic_load(&ra_ns);
				span = atomic_load(&ra_last_ns) - atomic_load(&ra_first_ns);
				if (span)
					os_log_info(bdev_log(),
						    "read window %{public}s: %llu calls, %llu MiB, mean %.2f ms/call, "
						    "device busy %.1f%% of the span, %.0f MB/s while busy, %.0f MB/s overall",
						    dev->name, calls, bytes >> 20,
						    (double)busy / calls / 1e6,
						    100.0 * (double)busy / (double)span,
						    (double)bytes / 1048576.0 / ((double)busy / 1e9),
						    (double)bytes / 1048576.0 / ((double)span / 1e9));
				atomic_store(&ra_calls, 0); atomic_store(&ra_bytes, 0);
				atomic_store(&ra_ns, 0); atomic_store(&ra_first_ns, 0);
			}
		}
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
	ntfs_bdev_detach_mapping(dev);	/* before priv: writeback goes through it */
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
	/* Kernel code reads ahead and writes back through bd_mapping; without one
	 * ntfs_empty_logfile() dereferences NULL on the first read-write mount of
	 * a volume whose journal is not empty. */
	if (ntfs_bdev_attach_mapping(dev) != 0) {
		os_log_error(bdev_log(), "%{public}s: no device page cache", dev->name);
		fskit_close(dev);
		return NULL;
	}
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
