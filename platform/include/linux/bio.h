/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Synchronous bio emulation. A bio collects up to BIO_MAX_VECS segments of
 * folio memory; submit_bio()/submit_bio_wait() perform the I/O immediately
 * through the ntfs_bdev vtable and then call bi_end_io. Implemented in
 * platform/src/bio.c.
 */
#ifndef _LINUX_BIO_H
#define _LINUX_BIO_H
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/mm_types.h>

#define BIO_MAX_VECS 256
#define REQ_OP_READ 0
#define REQ_OP_WRITE 1
#define REQ_OP_MASK 0xff
#define REQ_SYNC (1u << 8)
#define REQ_META (1u << 9)
#define REQ_PRIO (1u << 10)
#define REQ_FUA (1u << 11)
#define REQ_PREFLUSH (1u << 12)
#define REQ_IDLE (1u << 13)
typedef unsigned int blk_opf_t;
typedef u8 blk_status_t;
#define BLK_STS_OK 0
#define BLK_STS_IOERR 10

struct bio_vec { void *addr; unsigned int len; };
struct bvec_iter { sector_t bi_sector; unsigned int bi_size; };

struct bio {
	struct ntfs_bdev *bi_bdev;
	blk_opf_t bi_opf;
	struct bvec_iter bi_iter;
	void *bi_private;
	void (*bi_end_io)(struct bio *bio);
	blk_status_t bi_status;
	unsigned int bi_vcnt;
	unsigned int bi_max_vecs;
	struct bio *bi_next;	/* bio_chain */
	struct bio_vec bi_io_vec[BIO_MAX_VECS];
};

struct bio *bio_alloc(struct ntfs_bdev *bdev, unsigned int nr_vecs, blk_opf_t opf, gfp_t gfp);
void bio_put(struct bio *bio);
bool bio_add_folio(struct bio *bio, struct folio *folio, size_t len, size_t off);
int bio_add_page(struct bio *bio, struct page *page, unsigned int len, unsigned int off);
int bio_add_vmalloc_chunk(struct bio *bio, void *vaddr, unsigned int len);
void submit_bio(struct bio *bio);
int submit_bio_wait(struct bio *bio);
void bio_chain(struct bio *bio, struct bio *parent);
static inline sector_t bio_end_sector(const struct bio *bio) { return bio->bi_iter.bi_sector + (bio->bi_iter.bi_size >> SECTOR_SHIFT); }
static inline unsigned int bio_max_segs(unsigned int n) { return n > BIO_MAX_VECS ? BIO_MAX_VECS : n; }
static inline int blk_status_to_errno(blk_status_t s) { return s ? -EIO : 0; }
static inline blk_status_t errno_to_blk_status(int e) { return e ? BLK_STS_IOERR : BLK_STS_OK; }
#define bio_op(bio) ((bio)->bi_opf & REQ_OP_MASK)
#endif
