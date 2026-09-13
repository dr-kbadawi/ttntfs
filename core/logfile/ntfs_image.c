// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ntfs_image.h"
#include "logfile_layout.h"

static int seterr(struct ntfs_image *img, int err, const char *msg)
{
	snprintf(img->err, sizeof(img->err), "%s", msg);
	return err;
}

static int pread_all(int fd, uint64_t off, void *buf, size_t len);
static int pwrite_all(int fd, uint64_t off, const void *buf, size_t len);

static int img_fd_pread(void *ctx, uint64_t off, void *buf, size_t len)
{
	return pread_all(((struct ntfs_image *)ctx)->fd, off, buf, len);
}

static int img_fd_pwrite(void *ctx, uint64_t off, const void *buf, size_t len)
{
	return pwrite_all(((struct ntfs_image *)ctx)->fd, off, buf, len);
}

static int ntfs_image_parse(struct ntfs_image *img, uint32_t cluster_size,
			    uint32_t mft_record_size, bool allow_raw);

static int img_pread(struct ntfs_image *img, uint64_t off, void *buf, size_t len)
{
	return img->io.pread(img->io.ctx, off, buf, len);
}

static int img_pwrite(struct ntfs_image *img, uint64_t off, const void *buf, size_t len)
{
	if (!img->io.pwrite)
		return -EROFS;
	return img->io.pwrite(img->io.ctx, off, buf, len);
}

static int pread_all(int fd, uint64_t off, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len) {
		ssize_t n = pread(fd, p, len, (off_t)off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!n)
			return -EIO;
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

static int pwrite_all(int fd, uint64_t off, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len) {
		ssize_t n = pwrite(fd, p, len, (off_t)off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

uint64_t ntfs_image_map(const struct ntfs_img_run *runs, uint32_t n, uint32_t cs, uint64_t vbo)
{
	uint64_t vcn = vbo / cs;
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].len) {
			if (runs[i].lcn == UINT64_MAX)
				return UINT64_MAX;
			return (runs[i].lcn + (vcn - runs[i].vcn)) * cs + vbo % cs;
		}
	}
	return UINT64_MAX;
}

static int decode_runs(const uint8_t *mp, uint32_t mp_len, uint64_t svcn,
		       struct ntfs_img_run **out, uint32_t *n)
{
	uint64_t vcn = svcn;
	int64_t lcn = 0;
	uint32_t i = 0, cap = 0;

	*out = NULL;
	*n = 0;
	while (i < mp_len && mp[i]) {
		uint8_t h = mp[i++], ls = h & 0xf, os = h >> 4;
		uint64_t len = 0;
		int64_t d = 0;
		uint32_t k;
		if (!ls || ls > 8 || os > 8 || i + ls + os > mp_len)
			return -EINVAL;
		for (k = 0; k < ls; k++)
			len |= (uint64_t)mp[i + k] << (8 * k);
		i += ls;
		if (os) {
			for (k = 0; k < os; k++)
				d |= (int64_t)mp[i + k] << (8 * k);
			if (mp[i + os - 1] & 0x80)
				d |= -((int64_t)1 << (8 * os));
			i += os;
			lcn += d;
			if (lcn < 0)
				return -EINVAL;
		}
		if (*n == cap) {
			cap = cap ? cap * 2 : 8;
			struct ntfs_img_run *nr = realloc(*out, cap * sizeof(*nr));
			if (!nr)
				return -ENOMEM;
			*out = nr;
		}
		(*out)[*n].vcn = vcn;
		(*out)[*n].lcn = os ? (uint64_t)lcn : UINT64_MAX;
		(*out)[*n].len = len;
		(*n)++;
		vcn += len;
	}
	return 0;
}

/* Read MFT record @no through the $MFT run list (record 0 via mft_lcn). */
static int read_mft_record(struct ntfs_image *img, uint64_t no, uint8_t *rec)
{
	uint64_t off;
	int err;
	bool torn;

	if (!img->n_mft_runs)
		off = img->mft_lcn * img->cluster_size + no * img->mft_record_size;
	else
		off = ntfs_image_map(img->mft_runs, img->n_mft_runs, img->cluster_size,
				     no * img->mft_record_size);
	if (off == UINT64_MAX)
		return -ENOENT;
	err = img_pread(img, off, rec, img->mft_record_size);
	if (err)
		return err;
	if (lf_get32(rec) != LFS_MAGIC_FILE)
		return -EINVAL;
	err = ntfs_log_fixup_post_read(rec, img->mft_record_size, LFS_SECTOR_SIZE, &torn);
	if (err || torn)
		return -EINVAL;
	return 0;
}

static int find_data_runs(struct ntfs_image *img, const uint8_t *rec, struct ntfs_img_run **runs,
			  uint32_t *n, uint64_t *data_size)
{
	uint32_t o = lf_get16(rec + MR_ATTRS_OFFSET), rs = img->mft_record_size;

	while (o + AT_RESIDENT_SIZE <= rs) {
		uint32_t type = lf_get32(rec + o + AT_TYPE), len = lf_get32(rec + o + AT_LENGTH);
		if (type == ATTR_TYPE_END)
			break;
		if (len < AT_RESIDENT_SIZE || (len & 7) || o + len > rs)
			return -EINVAL;
		if (type == ATTR_TYPE_DATA && !rec[o + AT_NAME_LENGTH]) {
			uint16_t mpo;
			if (!rec[o + AT_NON_RESIDENT])
				return -EINVAL;
			mpo = lf_get16(rec + o + AT_MAPPING_PAIRS_OFFSET);
			if (mpo >= len)
				return -EINVAL;
			*data_size = lf_get64(rec + o + AT_DATA_SIZE);
			return decode_runs(rec + o + mpo, len - mpo, lf_get64(rec + o + AT_LOWEST_VCN), runs, n);
		}
		o += len;
	}
	return -ENOENT;
}

/*
 * Parse the volume: boot sector geometry, then the $MFT and $LogFile run lists.
 * Split out of ntfs_image_open() so a caller that already has the bytes -- the
 * driver, over its own block device -- gets the same parser rather than a
 * second, subtly different NTFS reader.
 */
int ntfs_image_open_io(struct ntfs_image *img, const struct ntfs_image_io *io,
		       uint64_t size, bool writable)
{
	if (!img || !io || !io->pread)
		return -EINVAL;
	memset(img, 0, sizeof(*img));
	img->fd = -1;
	img->io = *io;
	img->writable = writable;
	img->file_size = size;
	return ntfs_image_parse(img, 0, 0, false);
}

/*
 * Geometry and run lists from the bytes already reachable through img->io.
 * Shared by both entry points so a volume is read exactly one way.
 */
static int ntfs_image_parse(struct ntfs_image *img, uint32_t cluster_size,
			    uint32_t mft_record_size, bool allow_raw)
{
	uint8_t bs[512];
	int err;
	uint32_t magic;
	int8_t cpr;
	uint8_t *rec;

	if (img->file_size < 512)
		return seterr(img, -EINVAL, "volume too small");
	err = img_pread(img, 0, bs, sizeof(bs));
	if (err)
		return seterr(img, err, "cannot read first sector");
	magic = lf_get32(bs);
	if (allow_raw &&
	    (magic == LFS_MAGIC_RSTR || magic == LFS_MAGIC_CHKD || magic == LFS_MAGIC_EMPTY ||
	     magic == LFS_MAGIC_RCRD)) {
		img->raw = true;
		img->cluster_size = cluster_size ? cluster_size : 4096;
		img->mft_record_size = mft_record_size ? mft_record_size : 1024;
		img->sector_size = 512;
		img->log_size = img->file_size;
		return 0;
	}
	if (memcmp(bs + 3, "NTFS    ", 8))
		return seterr(img, -EINVAL, "not an NTFS boot sector and not a $LogFile dump");
	img->sector_size = lf_get16(bs + 0x0b);
	img->cluster_size = img->sector_size * bs[0x0d];
	if (bs[0x0d] > 0x80)	/* Windows 10+: sectors per cluster as a power of two */
		img->cluster_size = img->sector_size * (1u << (256 - bs[0x0d]));
	img->total_sectors = lf_get64(bs + 0x28);
	img->mft_lcn = lf_get64(bs + 0x30);
	img->mftmirr_lcn = lf_get64(bs + 0x38);
	cpr = (int8_t)bs[0x40];
	img->mft_record_size = cpr < 0 ? 1u << -cpr : (uint32_t)cpr * img->cluster_size;
	cpr = (int8_t)bs[0x44];
	img->index_block_size = cpr < 0 ? 1u << -cpr : (uint32_t)cpr * img->cluster_size;
	if (!img->sector_size || !img->cluster_size || img->mft_record_size < 512 ||
	    img->mft_record_size > 65536 || (img->mft_record_size & 511))
		return seterr(img, -EINVAL, "implausible boot sector geometry");

	rec = malloc(img->mft_record_size);
	if (!rec)
		return -ENOMEM;
	err = read_mft_record(img, 0, rec);
	if (err) {
		free(rec);
		return seterr(img, err, "cannot read MFT record 0");
	}
	{
		uint64_t ds;
		err = find_data_runs(img, rec, &img->mft_runs, &img->n_mft_runs, &ds);
	}
	if (err) {
		free(rec);
		return seterr(img, err, "MFT record 0: no usable $DATA run list");
	}
	err = read_mft_record(img, 2, rec);
	if (err) {
		free(rec);
		return seterr(img, err, "cannot read MFT record 2 ($LogFile)");
	}
	err = find_data_runs(img, rec, &img->log_runs, &img->n_log_runs, &img->log_size);
	free(rec);
	if (err)
		return seterr(img, err, "$LogFile: no usable $DATA run list");
	return 0;
}

int ntfs_image_open(struct ntfs_image *img, const char *path, bool writable,
		    uint32_t cluster_size, uint32_t mft_record_size)
{
	struct stat st;

	memset(img, 0, sizeof(*img));
	img->io.ctx = img;
	img->io.pread = img_fd_pread;
	img->io.pwrite = writable ? img_fd_pwrite : NULL;
	img->io.sync = NULL;
	img->fd = open(path, writable ? O_RDWR : O_RDONLY);
	if (img->fd < 0)
		return seterr(img, -errno, "cannot open file");
	img->writable = writable;
	if (fstat(img->fd, &st))
		return seterr(img, -errno, "fstat failed");
	img->file_size = (uint64_t)st.st_size;
	return ntfs_image_parse(img, cluster_size, mft_record_size, true);
}

void ntfs_image_close(struct ntfs_image *img)
{
	if (img->fd >= 0)
		close(img->fd);
	img->fd = -1;
	free(img->mft_runs);
	free(img->log_runs);
	img->mft_runs = img->log_runs = NULL;
}

/* ---- ntfs_log_io ------------------------------------------------------- */

static int log_rw(struct ntfs_image *img, uint64_t off, void *buf, size_t len, bool write)
{
	uint8_t *p = buf;

	if (img->raw)
		return write ? img_pwrite(img, off, buf, len) : img_pread(img, off, buf, len);
	while (len) {
		uint64_t phys = ntfs_image_map(img->log_runs, img->n_log_runs, img->cluster_size, off);
		size_t n = img->cluster_size - (size_t)(off % img->cluster_size);
		int err;
		if (n > len)
			n = len;
		if (phys == UINT64_MAX) {
			if (write)
				return -EIO;
			memset(p, 0xff, n);
		} else {
			err = write ? img_pwrite(img, phys, p, n) : img_pread(img, phys, p, n);
			if (err)
				return err;
		}
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static int io_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	return log_rw(ctx, off, buf, len, false);
}

static int io_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	return log_rw(ctx, off, (void *)buf, len, true);
}

void ntfs_image_log_io(struct ntfs_image *img, struct ntfs_log_io *io)
{
	memset(io, 0, sizeof(*io));
	io->ctx = img;
	io->size = img->log_size;
	io->read = io_read;
	io->write = img->writable ? io_write : NULL;
}

void ntfs_image_geometry(const struct ntfs_image *img, struct ntfs_log_geometry *g)
{
	g->cluster_size = img->cluster_size;
	g->mft_record_size = img->mft_record_size;
	g->sector_size = img->sector_size;
}

/* ---- ntfs_log_apply ---------------------------------------------------- */

static int ap_read_mft(void *ctx, uint64_t no, void *buf)
{
	struct ntfs_image *img = ctx;
	uint64_t off = ntfs_image_map(img->mft_runs, img->n_mft_runs, img->cluster_size,
				      no * img->mft_record_size);
	if (off == UINT64_MAX)
		return -ENOENT;
	return img_pread(img, off, buf, img->mft_record_size);
}

static int ap_write_mft(void *ctx, uint64_t no, const void *buf)
{
	struct ntfs_image *img = ctx;
	uint64_t off = ntfs_image_map(img->mft_runs, img->n_mft_runs, img->cluster_size,
				      no * img->mft_record_size);
	if (off == UINT64_MAX)
		return -ENOENT;
	img->writes_mft++;
	return img_pwrite(img, off, buf, img->mft_record_size);
}

static int ap_read_clusters(void *ctx, uint64_t lcn, uint32_t count, void *buf)
{
	struct ntfs_image *img = ctx;
	return img_pread(img, lcn * img->cluster_size, buf, (size_t)count * img->cluster_size);
}

static int ap_write_clusters(void *ctx, uint64_t lcn, uint32_t count, const void *buf)
{
	struct ntfs_image *img = ctx;
	img->writes_clusters += count;
	return img_pwrite(img, lcn * img->cluster_size, buf, (size_t)count * img->cluster_size);
}

static int ap_sync(void *ctx)
{
	struct ntfs_image *img = ctx;
	if (img->io.sync)
		return img->io.sync(img->io.ctx);
	return fsync(img->fd) ? -errno : 0;
}

void ntfs_image_apply(struct ntfs_image *img, struct ntfs_log_apply *ap)
{
	memset(ap, 0, sizeof(*ap));
	ap->ctx = img;
	ap->read_mft_record = ap_read_mft;
	ap->read_clusters = ap_read_clusters;
	if (img->writable) {
		ap->write_mft_record = ap_write_mft;
		ap->write_clusters = ap_write_clusters;
		ap->sync = ap_sync;
	}
}
