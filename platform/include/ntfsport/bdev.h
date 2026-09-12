/* SPDX-License-Identifier: GPL-2.0 */
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
	char name[64];
	void *priv;
	/* Raw-device page cache mapping. Always empty in the port (I/O goes
	 * straight through the vtable); exists so kernel code that flushes or
	 * reads ahead on bdev->bd_mapping keeps working. Owned by bdev_file.c. */
	struct address_space *bd_mapping;
};

/* Convenience wrappers used by the core; return 0 or negative errno. */
int ntfs_bdev_read(struct ntfs_bdev *dev, void *buf, u64 offset, size_t count);
int ntfs_bdev_write(struct ntfs_bdev *dev, const void *buf, u64 offset, size_t count);
int ntfs_bdev_flush(struct ntfs_bdev *dev);
int ntfs_bdev_discard(struct ntfs_bdev *dev, u64 offset, u64 length);

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
