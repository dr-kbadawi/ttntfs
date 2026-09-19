/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Block device abstraction for the user-space port. CONTRACT HEADER.
 *
 * Replaces the kernel's struct block_device / bio. Every byte the driver
 * reads or writes goes through this vtable: the page cache backend
 * (core/vfs aops), the MFT mirror sync, boot-sector loading, and the file
 * data fast path. Implementations:
 *   platform/src/bdev_file.c  - a plain file or /dev node via pread/pwrite
 *   fskit/                    - FSBlockDeviceResource (Swift/ObjC bridge)
 *
 * Offsets and lengths are in bytes. Implementations may require them to be
 * multiples of logical_block_size; callers in the port always satisfy that
 * (the page cache issues PAGE_SIZE-aligned I/O, the data path aligns edges).
 */
#ifndef NTFSPORT_BDEV_H
#define NTFSPORT_BDEV_H

#include <linux/types.h>

struct ntfs_bdev;

struct ntfs_bdev_ops {
	/* Return bytes transferred, or negative errno. Short transfers are
	 * errors for the caller; implementations should loop internally. */
	ssize_t (*pread)(struct ntfs_bdev *dev, void *buf, size_t count, u64 offset);
	ssize_t (*pwrite)(struct ntfs_bdev *dev, const void *buf, size_t count, u64 offset);
	/* Flush device write cache. */
	int (*flush)(struct ntfs_bdev *dev);
	/* Optional: TRIM/UNMAP. NULL if unsupported. */
	int (*discard)(struct ntfs_bdev *dev, u64 offset, u64 length);
	/* Release resources. */
	void (*close)(struct ntfs_bdev *dev);
};

struct ntfs_bdev {
	const struct ntfs_bdev_ops *ops;
	u64 size_bytes;
	u32 logical_block_size;		/* 512 or 4096 */
	u32 physical_block_size;
	u32 discard_granularity;	/* 0 = no discard */
	bool read_only;
	/* Set once a device has refused a flush, so the warning is logged once
	 * instead of on every sync. See fskit_flush() for why it is not fatal. */
	bool flush_warned;
	char name[64];
	void *priv;
	/* Raw-device page cache mapping, used by kernel code that flushes or
	 * reads ahead on bdev->bd_mapping (ntfs_empty_logfile does both). Set up
	 * by ntfs_bdev_attach_mapping(); every bdev implementation must call it,
	 * because nothing that uses this field checks it for NULL. */
	struct address_space *bd_mapping;
	/*
	 * Sticky: the last errno the *device* itself reported, set by
	 * ntfs_bdev_read()/ntfs_bdev_write() when ops->pread/pwrite fails or
	 * transfers short, and cleared by whoever wants a fresh window (mount
	 * does, per attempt). Deliberately not set by this layer's own range
	 * check, which means the caller asked past the end of the device rather
	 * than that the device is failing.
	 *
	 * It exists because failure paths in the core collapse many causes into
	 * one errno: ntfs_fill_super() returns -EINVAL for everything, and
	 * -EINVAL is what Disk Arbitration reads as "not an NTFS volume, try
	 * another driver". This flag is how that path tells a failing disk apart
	 * from a partition that genuinely is not NTFS.
	 */
	int io_err;
};

/* Convenience wrappers used by the core; return 0 or negative errno. */
int ntfs_bdev_read(struct ntfs_bdev *dev, void *buf, u64 offset, size_t count);
int ntfs_bdev_write(struct ntfs_bdev *dev, const void *buf, u64 offset, size_t count);
int ntfs_bdev_flush(struct ntfs_bdev *dev);
int ntfs_bdev_discard(struct ntfs_bdev *dev, u64 offset, u64 length);

/* Give @dev its bd_mapping page cache, or release it. Call attach from every
 * bdev constructor and detach from its destructor. */
int ntfs_bdev_attach_mapping(struct ntfs_bdev *dev);
void ntfs_bdev_detach_mapping(struct ntfs_bdev *dev);

static inline u64 bdev_nr_bytes(const struct ntfs_bdev *dev) { return dev->size_bytes; }
static inline u32 bdev_logical_block_size(const struct ntfs_bdev *dev) { return dev->logical_block_size; }
static inline u32 bdev_physical_block_size(const struct ntfs_bdev *dev) { return dev->physical_block_size; }
static inline u32 bdev_discard_granularity(const struct ntfs_bdev *dev) { return dev->discard_granularity; }
static inline u64 bdev_max_discard_sectors(const struct ntfs_bdev *dev) { return dev->discard_granularity ? (u64)-1 : 0; }

/* additive (compat stream): release through the vtable; NULL-safe. */
static inline void ntfs_bdev_close(struct ntfs_bdev *dev) { if (dev && dev->ops && dev->ops->close) dev->ops->close(dev); }

/* File/device-node backed implementation. */
struct ntfs_bdev *ntfs_bdev_open_path(const char *path, bool read_only);
/* Wrap an already-open file descriptor (not closed on close()). */
struct ntfs_bdev *ntfs_bdev_open_fd(int fd, bool read_only, const char *name);
/* additive (platform review): apply the raw-device I/O rules (requests must
 * be multiples of @block_size; others are bounced through a read-modify-
 * write) to a file-backed device. Tests use it to exercise that path on a
 * plain image; the backend also reports the new logical block size. */
void ntfs_bdev_file_set_alignment(struct ntfs_bdev *dev, u32 block_size);

#endif /* NTFSPORT_BDEV_H */
