/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tests for platform/src/bdev_file.c and bio.c over a temporary image file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <ntfsport/bdev.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define IMG_SIZE (1u << 20)

static char path[4096];

static void make_image(void)
{
	const char *tmp = getenv("TMPDIR");
	int fd;
	unsigned char *buf = malloc(IMG_SIZE);
	unsigned i;

	snprintf(path, sizeof(path), "%s/ntfs_test_bdev_XXXXXX", tmp ? tmp : "/tmp");
	fd = mkstemp(path);
	CHECK(fd >= 0);
	for (i = 0; i < IMG_SIZE; i++)
		buf[i] = (unsigned char)(i * 7 + (i >> 8));
	CHECK(write(fd, buf, IMG_SIZE) == IMG_SIZE);
	close(fd);
	free(buf);
}

static unsigned char expect(unsigned i) { return (unsigned char)(i * 7 + (i >> 8)); }

static void test_rw(void)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	unsigned char buf[8192], pat[777];
	unsigned i;

	CHECK(dev != NULL);
	if (!dev)
		return;
	CHECK(bdev_nr_bytes(dev) == IMG_SIZE);
	CHECK(bdev_logical_block_size(dev) == 512);
	CHECK(!dev->read_only);
	CHECK(dev->bd_mapping != NULL);

	/* Aligned and unaligned reads. */
	CHECK(ntfs_bdev_read(dev, buf, 4096, 4096) == 0);
	for (i = 0; i < 4096; i++)
		if (buf[i] != expect(4096 + i)) { CHECK(0); break; }
	CHECK(ntfs_bdev_read(dev, buf, 1234, 777) == 0);
	for (i = 0; i < 777; i++)
		if (buf[i] != expect(1234 + i)) { CHECK(0); break; }

	/* Unaligned write, read back, neighbours intact. */
	memset(pat, 0xAB, sizeof(pat));
	CHECK(ntfs_bdev_write(dev, pat, 100000, 777) == 0);
	CHECK(ntfs_bdev_read(dev, buf, 99990, 800) == 0);
	for (i = 0; i < 10; i++) CHECK(buf[i] == expect(99990 + i));
	for (i = 10; i < 787; i++) if (buf[i] != 0xAB) { CHECK(0); break; }
	for (i = 787; i < 800; i++) CHECK(buf[i] == expect(99990 + i));

	/* Bounds. */
	CHECK(ntfs_bdev_read(dev, buf, IMG_SIZE - 100, 200) == -EIO);
	CHECK(ntfs_bdev_read(dev, buf, IMG_SIZE, 1) == -EIO);
	CHECK(ntfs_bdev_write(dev, buf, IMG_SIZE - 100, 200) == -EIO);
	CHECK(ntfs_bdev_read(dev, buf, IMG_SIZE - 100, 100) == 0);
	CHECK(ntfs_bdev_read(dev, buf, 0, 0) == 0);

	CHECK(ntfs_bdev_flush(dev) == 0);
	{
		int d = ntfs_bdev_discard(dev, 65536, 65536);
		CHECK(d == 0 || d == -EOPNOTSUPP);
		CHECK(ntfs_bdev_discard(dev, IMG_SIZE, 4096) == -EINVAL);
	}

	/* bd_mapping: reads through the page cache see device bytes; the
	 * last folio beyond EOF is zero-filled. */
	{
		struct folio *f = read_mapping_folio(dev->bd_mapping, 3, NULL);
		CHECK(!IS_ERR(f));
		if (!IS_ERR(f)) {
			unsigned char *p = folio_address(f);
			for (i = 0; i < PAGE_SIZE; i++)
				if (p[i] != expect(3 * PAGE_SIZE + i)) { CHECK(0); break; }
			folio_put(f);
		}
		f = read_mapping_folio(dev->bd_mapping, IMG_SIZE / PAGE_SIZE + 5, NULL);
		CHECK(!IS_ERR(f));
		if (!IS_ERR(f)) {
			unsigned char *p = folio_address(f);
			for (i = 0; i < PAGE_SIZE; i++)
				if (p[i]) { CHECK(0); break; }
			folio_put(f);
		}
	}

	/* bio write through a standalone page invalidates bd_mapping. */
	{
		struct page *pg = alloc_page(GFP_KERNEL);
		struct bio *bio = bio_alloc(dev, 1, REQ_OP_WRITE, GFP_NOIO);
		struct folio *f;
		unsigned char *p;

		CHECK(pg && bio);
		memset(page_address(pg), 0x5C, PAGE_SIZE);
		bio->bi_iter.bi_sector = (3 * PAGE_SIZE) >> SECTOR_SHIFT;
		CHECK(bio_add_page(bio, pg, PAGE_SIZE, 0) == PAGE_SIZE);
		CHECK(submit_bio_wait(bio) == 0);
		bio_put(bio);
		put_page(pg);
		f = read_mapping_folio(dev->bd_mapping, 3, NULL);
		CHECK(!IS_ERR(f));
		if (!IS_ERR(f)) {
			p = folio_address(f);
			for (i = 0; i < PAGE_SIZE; i++)
				if (p[i] != 0x5C) { CHECK(0); break; }
			folio_put(f);
		}
		CHECK(ntfs_bdev_read(dev, buf, 3 * PAGE_SIZE, 512) == 0 && buf[0] == 0x5C && buf[511] == 0x5C);
	}

	/* vmap/vunmap: contiguous copy, write-back on unmap. */
	{
		struct page *pgs[3];
		char *v;
		int k;
		for (k = 0; k < 3; k++) {
			pgs[k] = alloc_page(GFP_KERNEL | __GFP_ZERO);
			memset(page_address(pgs[k]), 'a' + k, PAGE_SIZE);
		}
		v = vmap(pgs, 3, VM_MAP, PAGE_KERNEL);
		CHECK(v && v[0] == 'a' && v[PAGE_SIZE] == 'b' && v[2 * PAGE_SIZE + 5] == 'c');
		v[PAGE_SIZE + 1] = 'Z';
		vunmap(v);
		CHECK(((char *)page_address(pgs[1]))[1] == 'Z');
		for (k = 0; k < 3; k++)
			__free_page(pgs[k]);
	}

	ntfs_bdev_close(dev);
}

static void test_readonly_and_fd(void)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, true);
	unsigned char buf[512];
	int fd;

	CHECK(dev && dev->read_only);
	if (dev) {
		CHECK(ntfs_bdev_write(dev, buf, 0, 512) == -EROFS);
		CHECK(ntfs_bdev_read(dev, buf, 0, 512) == 0);
		ntfs_bdev_close(dev);
	}
	fd = open(path, O_RDWR);
	CHECK(fd >= 0);
	dev = ntfs_bdev_open_fd(fd, false, "fdtest");
	CHECK(dev && !strcmp(dev->name, "fdtest") && bdev_nr_bytes(dev) == IMG_SIZE);
	if (dev)
		ntfs_bdev_close(dev);
	CHECK(fcntl(fd, F_GETFD) != -1);	/* still open: not owned */
	close(fd);
	CHECK(ntfs_bdev_open_path("/nonexistent/ntfs/image", true) == NULL);
}

int main(void)
{
	make_image();
	test_rw();
	test_readonly_and_fd();
	unlink(path);
	pagecache_writeback_stop();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_bdev: ok\n");
	return 0;
}
