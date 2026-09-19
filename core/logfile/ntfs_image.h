/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Minimal read access to an NTFS image for the $LogFile tools: boot
 * sector geometry, the $MFT run list from record 0, the $LogFile run list
 * from record 2, and ntfs_log_io / ntfs_log_apply implementations over a
 * file descriptor. Not a general NTFS reader: attribute lists are not
 * followed (system files 0 and 2 never need one in practice).
 */
#ifndef NTFS_IMAGE_H
#define NTFS_IMAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "ntfs_logfile.h"

struct ntfs_img_run {
	uint64_t vcn, lcn, len;		/* lcn == UINT64_MAX: hole */
};

/*
 * Where the bytes come from. The tools open a file; the driver hands over its
 * own block device, which is the same flat address space and lets the core run
 * the analysis on a real volume without a second NTFS reader.
 */
struct ntfs_image_io {
	void *ctx;
	int (*pread)(void *ctx, uint64_t off, void *buf, size_t len);
	int (*pwrite)(void *ctx, uint64_t off, const void *buf, size_t len);   /* NULL: read-only */
	int (*sync)(void *ctx);                                                /* optional */
};

struct ntfs_image {
	struct ntfs_image_io io;
	int fd;
	bool writable;
	bool raw;			/* file is a raw $LogFile dump, not a volume */
	uint64_t file_size;
	uint32_t sector_size, cluster_size, mft_record_size, index_block_size;
	uint64_t mft_lcn, mftmirr_lcn, total_sectors;
	struct ntfs_img_run *mft_runs;
	uint32_t n_mft_runs;
	struct ntfs_img_run *log_runs;
	uint32_t n_log_runs;
	uint64_t log_size;		/* $LogFile data size */
	uint32_t writes_mft, writes_clusters;	/* counters for reporting */
	char err[256];
};

/* Open @path. If the first bytes are a restart page (or all 0xff) the file
 * is treated as a raw $LogFile dump; @cluster_size/@mft_record_size then
 * come from the arguments (0 = 4096/1024). Returns 0 or -errno. */
int ntfs_image_open(struct ntfs_image *img, const char *path, bool writable,
		    uint32_t cluster_size, uint32_t mft_record_size);

/* Same, over caller-supplied I/O: @io->pread must be set, @io->pwrite only for
 * a writable image. @size is the addressable length. Never a raw log dump. */
int ntfs_image_open_io(struct ntfs_image *img, const struct ntfs_image_io *io,
		       uint64_t size, bool writable);
void ntfs_image_close(struct ntfs_image *img);

/* Read $Volume (MFT record 3) and report the label (UTF-8, NUL-terminated),
 * the NTFS version and the volume flags. One MFT record is read; the volume is
 * not mounted. Any output may be NULL. Returns 0 if either attribute was found,
 * -ENOENT if neither, or -errno. */
int ntfs_image_volume_info(struct ntfs_image *img, char *label, size_t label_size,
			   uint8_t *major, uint8_t *minor, uint16_t *vol_flags);

/* Fill the module's I/O and apply vtables. */
void ntfs_image_log_io(struct ntfs_image *img, struct ntfs_log_io *io);
void ntfs_image_geometry(const struct ntfs_image *img, struct ntfs_log_geometry *g);
void ntfs_image_apply(struct ntfs_image *img, struct ntfs_log_apply *ap);

/* Byte offset of @vbo inside a run list, UINT64_MAX if unmapped/hole. */
uint64_t ntfs_image_map(const struct ntfs_img_run *runs, uint32_t n, uint32_t cluster_size, uint64_t vbo);

#endif
