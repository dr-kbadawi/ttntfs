/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ntfsport/bdev.h over a regular file or a /dev node (bdev_file.c).
 *
 *  - pread/pwrite loop until the whole request is done; raw devices get a
 *    bounce buffer when the request is not a multiple of the logical block
 *    size (macOS raw disks reject unaligned I/O).
 *  - flush is fcntl(F_FULLFSYNC) (falls back to fsync).
 *  - discard is F_PUNCHHOLE on regular files and DKIOCUNMAP on devices.
 *  - size/geometry via fstat, or DKIOCGETBLOCKSIZE/DKIOCGETBLOCKCOUNT/
 *    DKIOCGETPHYSICALBLOCKSIZE for devices.
 *  - bd_mapping is a page cache over the raw device (index = byte offset /
 *    PAGE_SIZE); compress.c reads compression blocks through it and
 *    logfile.c flushes it. Writes through the bio layer invalidate it.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <ntfsport/bdev.h>

struct bdev_file {
	int fd;
	bool owns_fd;
	bool is_device;
};

static inline struct bdev_file *bf(struct ntfs_bdev *dev) { return dev->priv; }

/* ------------------------------------------------------------------ */
/* Raw I/O                                                             */

static ssize_t do_pread(int fd, void *buf, size_t count, u64 offset)
{
	size_t done = 0;

	while (done < count) {
		ssize_t n = pread(fd, (char *)buf + done, count - done, (off_t)(offset + done));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;	/* short read: past the end */
		done += (size_t)n;
	}
	return (ssize_t)done;
}

static ssize_t do_pwrite(int fd, const void *buf, size_t count, u64 offset)
{
	size_t done = 0;

	while (done < count) {
		ssize_t n = pwrite(fd, (const char *)buf + done, count - done, (off_t)(offset + done));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		done += (size_t)n;
	}
	return (ssize_t)done;
}

static bool aligned_req(struct ntfs_bdev *dev, size_t count, u64 offset)
{
	u32 bs = dev->logical_block_size;
	return !bf(dev)->is_device || ((count % bs) == 0 && (offset % bs) == 0);
}

static ssize_t file_pread(struct ntfs_bdev *dev, void *buf, size_t count, u64 offset)
{
	if (aligned_req(dev, count, offset))
		return do_pread(bf(dev)->fd, buf, count, offset);

	/* Unaligned on a raw device: read the covering blocks and copy. */
	u32 bs = dev->logical_block_size;
	u64 start = round_down(offset, bs);
	u64 end = round_up(offset + count, bs);
	size_t len = (size_t)(end - start);
	void *tmp = NULL;
	ssize_t n;

	if (posix_memalign(&tmp, bs, len) != 0)
		return -ENOMEM;
	n = do_pread(bf(dev)->fd, tmp, len, start);
	if (n >= 0) {
		memcpy(buf, (char *)tmp + (offset - start), count);
		n = (ssize_t)count;
	}
	free(tmp);
	return n;
}

static ssize_t file_pwrite(struct ntfs_bdev *dev, const void *buf, size_t count, u64 offset)
{
	if (aligned_req(dev, count, offset))
		return do_pwrite(bf(dev)->fd, buf, count, offset);

	/* Unaligned on a raw device: read-modify-write the covering blocks. */
	u32 bs = dev->logical_block_size;
	u64 start = round_down(offset, bs);
	u64 end = round_up(offset + count, bs);
	size_t len = (size_t)(end - start);
	void *tmp = NULL;
	ssize_t n;

	if (posix_memalign(&tmp, bs, len) != 0)
		return -ENOMEM;
	n = do_pread(bf(dev)->fd, tmp, len, start);
	if (n >= 0) {
		memcpy((char *)tmp + (offset - start), buf, count);
		n = do_pwrite(bf(dev)->fd, tmp, len, start);
		if (n >= 0)
			n = (ssize_t)count;
	}
	free(tmp);
	return n;
}

static int file_flush(struct ntfs_bdev *dev)
{
	if (fcntl(bf(dev)->fd, F_FULLFSYNC) == 0)
		return 0;
	if (fsync(bf(dev)->fd) == 0)
		return 0;
	return errno == EINVAL || errno == ENOTSUP ? 0 : -errno;	/* not a syncable fd */
}

static int file_discard(struct ntfs_bdev *dev, u64 offset, u64 length)
{
	if (bf(dev)->is_device) {
		dk_extent_t ext = { .offset = offset, .length = length };
		dk_unmap_t um = { .extents = &ext, .extentsCount = 1, .options = 0 };
		if (ioctl(bf(dev)->fd, DKIOCUNMAP, &um) == 0)
			return 0;
		return errno == ENOTTY || errno == ENOTSUP ? -EOPNOTSUPP : -errno;
	} else {
		fpunchhole_t ph = { .fp_flags = 0, .reserved = 0,
				    .fp_offset = (off_t)offset, .fp_length = (off_t)length };
		if (fcntl(bf(dev)->fd, F_PUNCHHOLE, &ph) == 0)
			return 0;
		return errno == ENOTTY || errno == ENOTSUP || errno == EINVAL ? -EOPNOTSUPP : -errno;
	}
}

static void file_close(struct ntfs_bdev *dev)
{
	struct bdev_file *f = bf(dev);

	if (dev->bd_mapping) {
		address_space_destroy(dev->bd_mapping, false);
		free(dev->bd_mapping);
		dev->bd_mapping = NULL;
	}
	if (f) {
		if (f->owns_fd && f->fd >= 0)
			close(f->fd);
		free(f);
	}
	free(dev);
}

static const struct ntfs_bdev_ops file_ops = {
	.pread = file_pread,
	.pwrite = file_pwrite,
	.flush = file_flush,
	.discard = file_discard,
	.close = file_close,
};

/* ------------------------------------------------------------------ */
/* bd_mapping: page cache over the raw device                          */

static struct ntfs_bdev *mapping_bdev(struct address_space *m)
{
	return (struct ntfs_bdev *)m->private_list.next;	/* see open_common */
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

/* ------------------------------------------------------------------ */
/* Open                                                                */

static struct ntfs_bdev *open_common(int fd, bool owns_fd, bool read_only, const char *name)
{
	struct ntfs_bdev *dev;
	struct bdev_file *f;
	struct stat st;

	if (fstat(fd, &st) != 0)
		return NULL;
	dev = calloc(1, sizeof(*dev));
	f = calloc(1, sizeof(*f));
	if (!dev || !f) {
		free(dev);
		free(f);
		return NULL;
	}
	f->fd = fd;
	f->owns_fd = owns_fd;
	dev->ops = &file_ops;
	dev->priv = f;
	dev->read_only = read_only;
	if (name)
		strlcpy(dev->name, name, sizeof(dev->name));

	if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		uint32_t bs = 0, pbs = 0;
		uint64_t count = 0;

		f->is_device = true;
		if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) != 0 || !bs)
			bs = 512;
		if (ioctl(fd, DKIOCGETBLOCKCOUNT, &count) != 0)
			count = 0;
		if (ioctl(fd, DKIOCGETPHYSICALBLOCKSIZE, &pbs) != 0 || !pbs)
			pbs = bs;
		dev->logical_block_size = bs;
		dev->physical_block_size = pbs;
		dev->size_bytes = (u64)bs * count;
		dev->discard_granularity = bs;
	} else {
		dev->logical_block_size = 512;
		dev->physical_block_size = 4096;
		dev->size_bytes = (u64)st.st_size;
		dev->discard_granularity = 4096;
	}

	dev->bd_mapping = calloc(1, sizeof(*dev->bd_mapping));
	if (dev->bd_mapping) {
		address_space_init(dev->bd_mapping, NULL, &bdev_aops);
		/* The mapping has no host inode; stash the device in the
		 * (otherwise unused) private_list head. */
		dev->bd_mapping->private_list.next = (struct list_head *)dev;
		dev->bd_mapping->private_list.prev = (struct list_head *)dev;
	}
	return dev;
}

struct ntfs_bdev *ntfs_bdev_open_path(const char *path, bool read_only)
{
	int fd = -1;
	struct ntfs_bdev *dev;

	if (!read_only) {
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0 && (errno == EROFS || errno == EACCES || errno == EPERM))
			read_only = true;
	}
	if (fd < 0)
		fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	dev = open_common(fd, true, read_only, path);
	if (!dev)
		close(fd);
	return dev;
}

struct ntfs_bdev *ntfs_bdev_open_fd(int fd, bool read_only, const char *name)
{
	return open_common(fd, false, read_only, name);
}

/* ------------------------------------------------------------------ */
/* Wrappers                                                            */

int ntfs_bdev_read(struct ntfs_bdev *dev, void *buf, u64 offset, size_t count)
{
	ssize_t n;

	if (!count)
		return 0;
	if (offset > dev->size_bytes || count > dev->size_bytes - offset)
		return -EIO;
	n = dev->ops->pread(dev, buf, count, offset);
	if (n < 0)
		return (int)n;
	return (size_t)n == count ? 0 : -EIO;
}

int ntfs_bdev_write(struct ntfs_bdev *dev, const void *buf, u64 offset, size_t count)
{
	ssize_t n;

	if (!count)
		return 0;
	if (dev->read_only)
		return -EROFS;
	if (offset > dev->size_bytes || count > dev->size_bytes - offset)
		return -EIO;
	n = dev->ops->pwrite(dev, buf, count, offset);
	if (n < 0)
		return (int)n;
	return (size_t)n == count ? 0 : -EIO;
}

int ntfs_bdev_flush(struct ntfs_bdev *dev)
{
	if (!dev->ops->flush)
		return 0;
	return dev->ops->flush(dev);
}

int ntfs_bdev_discard(struct ntfs_bdev *dev, u64 offset, u64 length)
{
	if (!dev->ops->discard || !dev->discard_granularity)
		return -EOPNOTSUPP;
	if (dev->read_only)
		return -EROFS;
	if (!length)
		return 0;
	if (offset > dev->size_bytes || length > dev->size_bytes - offset)
		return -EINVAL;
	return dev->ops->discard(dev, offset, length);
}
