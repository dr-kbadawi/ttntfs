/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Synchronous bio emulation (linux/bio.h). mft.c writes MFT records and
 * the MFT mirror with bios; each bio is executed immediately through the
 * ntfs_bdev vtable when submitted. submit_bio() then calls bi_end_io (which
 * in mft.c drops the folio reference and puts the bio); submit_bio_wait()
 * returns the errno and leaves the bio to the caller, as in the kernel.
 *
 * A write through a bio invalidates the overlapping folios of the device's
 * bd_mapping so compress.c, which reads raw clusters through that mapping,
 * never sees stale data.
 */
#include <stdlib.h>
#include <string.h>
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>

struct bio *bio_alloc(struct ntfs_bdev *bdev, unsigned int nr_vecs, blk_opf_t opf, gfp_t gfp)
{
	struct bio *bio = calloc(1, sizeof(*bio));

	(void)nr_vecs;
	(void)gfp;
	if (!bio)
		return NULL;
	bio->bi_bdev = bdev;
	bio->bi_opf = opf;
	/* Callers pass a hint; a full-size vector table is cheap here and
	 * lets contiguous additions merge or append freely. */
	bio->bi_max_vecs = BIO_MAX_VECS;
	return bio;
}

void bio_put(struct bio *bio)
{
	free(bio);
}

static bool bio_add(struct bio *bio, void *addr, unsigned int len)
{
	struct bio_vec *bv;

	if (!len)
		return true;
	if (bio->bi_vcnt) {
		bv = &bio->bi_io_vec[bio->bi_vcnt - 1];
		if ((char *)bv->addr + bv->len == addr) {
			bv->len += len;
			bio->bi_iter.bi_size += len;
			return true;
		}
	}
	if (bio->bi_vcnt >= bio->bi_max_vecs)
		return false;
	bv = &bio->bi_io_vec[bio->bi_vcnt++];
	bv->addr = addr;
	bv->len = len;
	bio->bi_iter.bi_size += len;
	return true;
}

bool bio_add_folio(struct bio *bio, struct folio *folio, size_t len, size_t off)
{
	if (off + len > folio_size(folio))
		return false;
	return bio_add(bio, (char *)folio->data + off, (unsigned int)len);
}

int bio_add_page(struct bio *bio, struct page *page, unsigned int len, unsigned int off)
{
	if (!bio_add_folio(bio, page_folio(page), len, off))
		return 0;
	return (int)len;
}

int bio_add_vmalloc_chunk(struct bio *bio, void *vaddr, unsigned int len)
{
	return bio_add(bio, vaddr, len) ? (int)len : 0;
}

void bio_chain(struct bio *bio, struct bio *parent)
{
	bio->bi_next = parent;
}

static int bio_execute(struct bio *bio)
{
	struct ntfs_bdev *dev = bio->bi_bdev;
	u64 off = (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT;
	u64 start = off;
	unsigned int i;
	int err = 0;
	bool write = bio_op(bio) == REQ_OP_WRITE;

	if (!dev) {
		err = -ENODEV;
		goto done;
	}
	if (write && (bio->bi_opf & REQ_PREFLUSH))
		err = ntfs_bdev_flush(dev);
	for (i = 0; i < bio->bi_vcnt && !err; i++) {
		struct bio_vec *bv = &bio->bi_io_vec[i];

		err = write ? ntfs_bdev_write(dev, bv->addr, off, bv->len)
			    : ntfs_bdev_read(dev, bv->addr, off, bv->len);
		off += bv->len;
	}
	if (write && !err && dev->bd_mapping && bio->bi_iter.bi_size)
		invalidate_inode_pages2_range(dev->bd_mapping, start >> PAGE_SHIFT,
					      (start + bio->bi_iter.bi_size - 1) >> PAGE_SHIFT);
	if (write && !err && (bio->bi_opf & REQ_FUA))
		err = ntfs_bdev_flush(dev);
done:
	bio->bi_status = errno_to_blk_status(err);
	if (err && bio->bi_next && !bio->bi_next->bi_status)
		bio->bi_next->bi_status = bio->bi_status;
	return err;
}

void submit_bio(struct bio *bio)
{
	bio_execute(bio);
	if (bio->bi_end_io)
		bio->bi_end_io(bio);
}

int submit_bio_wait(struct bio *bio)
{
	return bio_execute(bio);
}
