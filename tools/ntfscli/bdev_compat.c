/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bdev_compat.c - minimal pread/pwrite implementation of ntfsport/bdev.h
 * for the stub build of ntfscli (NTFSCLI_USE_STUB). The real
 * implementation is platform/src/bdev_file.c inside libntfscore.a; this
 * file is only compiled when ntfscli links core/stub/ntfscore_stub.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/disk.h>
#endif

#include <ntfsport/bdev.h>

struct priv {
	int fd;
	bool owns_fd;
};

static ssize_t compat_pread(struct ntfs_bdev *dev, void *buf, size_t count, u64 offset)
{
	struct priv *p = dev->priv;
	size_t done = 0;
	while (done < count) {
		ssize_t n = pread(p->fd, (char *)buf + done, count - done, (off_t)(offset + done));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;	/* short read past end of device */
		done += (size_t)n;
	}
	return (ssize_t)done;
}

static ssize_t compat_pwrite(struct ntfs_bdev *dev, const void *buf, size_t count, u64 offset)
{
	struct priv *p = dev->priv;
	size_t done = 0;
	if (dev->read_only)
		return -EROFS;
	while (done < count) {
		ssize_t n = pwrite(p->fd, (const char *)buf + done, count - done, (off_t)(offset + done));
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

static int compat_flush(struct ntfs_bdev *dev)
{
	struct priv *p = dev->priv;
	if (dev->read_only)
		return 0;
#ifdef F_FULLFSYNC
	if (fcntl(p->fd, F_FULLFSYNC) == 0)
		return 0;
#endif
	return fsync(p->fd) ? -errno : 0;
}

static void compat_close(struct ntfs_bdev *dev)
{
	struct priv *p = dev->priv;
	if (p->owns_fd)
		close(p->fd);
	free(p);
	free(dev);
}

static const struct ntfs_bdev_ops compat_ops = {
	.pread = compat_pread,
	.pwrite = compat_pwrite,
	.flush = compat_flush,
	.discard = NULL,
	.close = compat_close,
};

struct ntfs_bdev *ntfs_bdev_open_fd(int fd, bool read_only, const char *name)
{
	struct stat st;
	if (fstat(fd, &st))
		return NULL;
	struct ntfs_bdev *dev = calloc(1, sizeof(*dev));
	struct priv *p = calloc(1, sizeof(*p));
	if (!dev || !p) {
		free(dev);
		free(p);
		errno = ENOMEM;
		return NULL;
	}
	p->fd = fd;
	dev->ops = &compat_ops;
	dev->priv = p;
	dev->read_only = read_only;
	dev->logical_block_size = 512;
	dev->physical_block_size = 512;
	dev->size_bytes = (u64)st.st_size;
	if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
#ifdef DKIOCGETBLOCKSIZE
		uint32_t bs = 0;
		uint64_t cnt = 0;
		if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) == 0 && bs)
			dev->logical_block_size = dev->physical_block_size = bs;
		if (ioctl(fd, DKIOCGETBLOCKCOUNT, &cnt) == 0)
			dev->size_bytes = cnt * dev->logical_block_size;
#endif
	}
	snprintf(dev->name, sizeof(dev->name), "%s", name ? name : "fd");
	return dev;
}

struct ntfs_bdev *ntfs_bdev_open_path(const char *path, bool read_only)
{
	int fd = open(path, read_only ? O_RDONLY : O_RDWR);
	if (fd < 0)
		return NULL;
	struct ntfs_bdev *dev = ntfs_bdev_open_fd(fd, read_only, path);
	if (!dev) {
		int e = errno;
		close(fd);
		errno = e;
		return NULL;
	}
	((struct priv *)dev->priv)->owns_fd = true;
	const char *base = strrchr(path, '/');
	snprintf(dev->name, sizeof(dev->name), "%s", base ? base + 1 : path);
	return dev;
}

int ntfs_bdev_read(struct ntfs_bdev *dev, void *buf, u64 offset, size_t count)
{
	ssize_t n = dev->ops->pread(dev, buf, count, offset);
	return n < 0 ? (int)n : 0;
}

int ntfs_bdev_write(struct ntfs_bdev *dev, const void *buf, u64 offset, size_t count)
{
	ssize_t n = dev->ops->pwrite(dev, buf, count, offset);
	return n < 0 ? (int)n : 0;
}

int ntfs_bdev_flush(struct ntfs_bdev *dev)
{
	return dev->ops->flush ? dev->ops->flush(dev) : 0;
}

int ntfs_bdev_discard(struct ntfs_bdev *dev, u64 offset, u64 length)
{
	return dev->ops->discard ? dev->ops->discard(dev, offset, length) : -EOPNOTSUPP;
}
