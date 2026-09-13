/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ntfsport/bdev.h over a regular file or a /dev node (bdev_file.c).
 *
 *  - pread/pwrite loop until the whole request is done (EINTR retried,
 *    at most IO_CHUNK bytes per call); raw devices get a bounce buffer
 *    when the request is not a multiple of the logical block size (macOS
 *    raw disks reject unaligned I/O). Sub-block writes are a
 *    read-modify-write of the covering blocks, serialised per device so
 *    two of them on the same block cannot lose each other's bytes.
 *  - flush is fcntl(F_FULLFSYNC); fsync only when the fd does not support
 *    it (an I/O error from F_FULLFSYNC is reported, not masked).
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
#include <pthread.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <ntfsport/bdev.h>

struct bdev_file {
	int fd;
	bool owns_fd;
	bool is_device;
	pthread_mutex_t rmw_lock;	/* unaligned read-modify-write */
};

/* Largest single pread/pwrite: the syscalls reject counts > INT_MAX. */
#define IO_CHUNK	(1UL << 30)

static inline struct bdev_file *bf(struct ntfs_bdev *dev) { return dev->priv; }

/* ------------------------------------------------------------------ */
/* Raw I/O                                                             */

static ssize_t do_pread(int fd, void *buf, size_t count, u64 offset)
{
	size_t done = 0;

	while (done < count) {
		ssize_t n = pread(fd, (char *)buf + done, min(count - done, IO_CHUNK),
				  (off_t)(offset + done));
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
		ssize_t n = pwrite(fd, (const char *)buf + done, min(count - done, IO_CHUNK),
				   (off_t)(offset + done));
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
	pthread_mutex_lock(&bf(dev)->rmw_lock);
	n = do_pread(bf(dev)->fd, tmp, len, start);
	if (n >= 0) {
		memcpy((char *)tmp + (offset - start), buf, count);
		n = do_pwrite(bf(dev)->fd, tmp, len, start);
		if (n >= 0)
			n = (ssize_t)count;
	}
	pthread_mutex_unlock(&bf(dev)->rmw_lock);
	free(tmp);
	return n;
}

static int file_flush(struct ntfs_bdev *dev)
{
	if (fcntl(bf(dev)->fd, F_FULLFSYNC) == 0)
		return 0;
	/* Only fall back when the file system does not implement it; an
	 * I/O error means the device may not have the data. */
	if (errno != ENOTSUP && errno != ENOTTY && errno != EINVAL && errno != EOPNOTSUPP)
		return -errno;
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

	ntfs_bdev_detach_mapping(dev);
	if (f) {
		if (f->owns_fd && f->fd >= 0)
			close(f->fd);
		pthread_mutex_destroy(&f->rmw_lock);
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
	pthread_mutex_init(&f->rmw_lock, NULL);
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
		if (!count) {
			/* Not a disk driver node (or the ioctl is refused):
			 * the end of the device is the next best answer. */
			off_t end = lseek(fd, 0, SEEK_END);
			if (end > 0)
				count = (uint64_t)end / bs;
		}
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

	ntfs_bdev_attach_mapping(dev);	/* best effort; see bdev_mapping.c */
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

void ntfs_bdev_file_set_alignment(struct ntfs_bdev *dev, u32 block_size)
{
	if (!dev || dev->ops != &file_ops || !block_size)
		return;
	bf(dev)->is_device = true;
	dev->logical_block_size = block_size;
	if (dev->physical_block_size < block_size)
		dev->physical_block_size = block_size;
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
