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
#include <pthread.h>
#include <stdint.h>
#include <time.h>
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

/* --- unaligned read-modify-write on a "raw device" ---------------------- */

/*
 * Sub-block writes to a raw device are bounced through a read-modify-write
 * of the covering blocks. Two threads updating different bytes of the same
 * block must not lose each other's update, so the RMW has to be serialised
 * per device.
 */
struct rmw_arg { struct ntfs_bdev *dev; int lane; int iters; int fails; };

static void *rmw_main(void *arg)
{
	struct rmw_arg *a = arg;
	unsigned char pat[64], back[64];
	u64 off = 8192 + (u64)a->lane * 64;		/* all lanes in block 8192.. */
	int i;

	for (i = 0; i < a->iters; i++) {
		memset(pat, (a->lane << 4) | (i & 15), sizeof(pat));
		if (ntfs_bdev_write(a->dev, pat, off, sizeof(pat)) != 0 ||
		    ntfs_bdev_read(a->dev, back, off, sizeof(back)) != 0 ||
		    memcmp(pat, back, sizeof(pat)) != 0)
			a->fails++;
	}
	return NULL;
}

static void test_rmw_race(void)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	enum { LANES = 8 };
	pthread_t th[LANES];
	struct rmw_arg a[LANES];
	unsigned char blk[4096];
	int i;

	CHECK(dev != NULL);
	if (!dev)
		return;
	ntfs_bdev_file_set_alignment(dev, 4096);
	CHECK(bdev_logical_block_size(dev) == 4096);
	/* Sub-block and misaligned requests still work (bounced). */
	CHECK(ntfs_bdev_read(dev, blk, 4096 + 100, 300) == 0);
	for (i = 0; i < 300; i++)
		if (blk[i] != expect(4096 + 100 + i)) { CHECK(0); break; }
	for (i = 0; i < LANES; i++) {
		a[i].dev = dev; a[i].lane = i; a[i].iters = 2000; a[i].fails = 0;
		pthread_create(&th[i], NULL, rmw_main, &a[i]);
	}
	for (i = 0; i < LANES; i++) {
		pthread_join(th[i], NULL);
		CHECK(a[i].fails == 0);
	}
	/* Final state: every lane holds its last pattern; the rest of the
	 * block is untouched. */
	CHECK(ntfs_bdev_read(dev, blk, 8192, 4096) == 0);
	for (i = 0; i < LANES; i++) {
		unsigned char want = (unsigned char)((i << 4) | ((a[i].iters - 1) & 15));
		int k;
		for (k = 0; k < 64; k++)
			if (blk[i * 64 + k] != want) { CHECK(0); break; }
	}
	for (i = LANES * 64; i < 4096; i++)
		if (blk[i] != expect(8192 + i)) { CHECK(0); break; }
	ntfs_bdev_close(dev);
}

/* --- workqueues and wait queues ---------------------------------------- */

#include <linux/workqueue.h>
#include <linux/wait.h>

struct selffree_work { struct work_struct work; int *ran; };

/* The kernel allows a work function to free its own work_struct (the
 * queue must not touch it after the function returns). */
static void selffree_fn(struct work_struct *w)
{
	struct selffree_work *sw = container_of(w, struct selffree_work, work);
	__atomic_add_fetch(sw->ran, 1, __ATOMIC_SEQ_CST);
	free(sw);
}

struct slow_work { struct work_struct work; int started, done; int runs; };

static void slow_fn(struct work_struct *w)
{
	struct slow_work *sw = container_of(w, struct slow_work, work);
	__atomic_store_n(&sw->started, 1, __ATOMIC_SEQ_CST);
	usleep(20000);
	sw->runs++;
	__atomic_store_n(&sw->done, 1, __ATOMIC_SEQ_CST);
}

static void test_work(void)
{
	struct workqueue_struct *wq = alloc_workqueue("test-%d", 0, 1, 7);
	struct slow_work sw;
	int ran = 0, i;

	CHECK(wq != NULL);
	/* Self-freeing work items: use-after-free shows under ASan. */
	for (i = 0; i < 16; i++) {
		struct selffree_work *w = malloc(sizeof(*w));
		INIT_WORK(&w->work, selffree_fn);
		w->ran = &ran;
		CHECK(queue_work(wq, &w->work));
	}
	flush_workqueue(wq);
	CHECK(ran == 16);

	/* cancel_work_sync() while running waits for the function. */
	memset(&sw, 0, sizeof(sw));
	INIT_WORK(&sw.work, slow_fn);
	CHECK(queue_work(wq, &sw.work));
	while (!__atomic_load_n(&sw.started, __ATOMIC_SEQ_CST))
		usleep(100);
	CHECK(cancel_work_sync(&sw.work) == false);	/* was running, not pending */
	CHECK(__atomic_load_n(&sw.done, __ATOMIC_SEQ_CST) == 1);
	CHECK(sw.runs == 1);
	/* Pending item is dequeued; flush_work of an idle item is a no-op. */
	sw.started = sw.done = 0;
	destroy_workqueue(wq);
	wq = alloc_workqueue("test2", 0, 1);
	/* Queue while running -> runs once more after the current run. */
	CHECK(queue_work(wq, &sw.work));
	while (!__atomic_load_n(&sw.started, __ATOMIC_SEQ_CST))
		usleep(100);
	CHECK(queue_work(wq, &sw.work) == true);
	CHECK(queue_work(wq, &sw.work) == false);	/* already pending */
	flush_work(&sw.work);
	CHECK(sw.runs == 3);
	CHECK(flush_work(&sw.work) == false);
	CHECK(cancel_work_sync(&sw.work) == false);
	destroy_workqueue(wq);
	/* schedule_work on the system queue. */
	{
		struct selffree_work *w = malloc(sizeof(*w));
		INIT_WORK(&w->work, selffree_fn);
		w->ran = &ran;
		CHECK(schedule_work(&w->work));
		flush_workqueue(system_wq);
		CHECK(ran == 17);
	}
}

struct waiter_arg { wait_queue_head_t *q; int *flag; long worst_us; int iters; };

static void *waiter_main(void *arg)
{
	struct waiter_arg *w = arg;
	int i;

	for (i = 0; i < w->iters; i++) {
		struct timespec t0, t1;
		long us;
		wait_event(*w->q, __atomic_load_n(w->flag, __ATOMIC_SEQ_CST) >= i + 1);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		/* the setter stamps t0 just before wake_up */
		t0 = *(struct timespec *)(w + 1);
		us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
		if (us > w->worst_us)
			w->worst_us = us;
	}
	return NULL;
}

/* A state change followed by wake_up() must be seen promptly (the poll
 * fallback is 100 ms; a lost wake-up would show as ~100 ms latencies). */
static void test_wait(void)
{
	wait_queue_head_t q;
	int flag = 0, i;
	struct { struct waiter_arg a; struct timespec t0; } w;
	pthread_t th;

	init_waitqueue_head(&q);
	w.a.q = &q; w.a.flag = &flag; w.a.worst_us = 0; w.a.iters = 2000;
	pthread_create(&th, NULL, waiter_main, &w.a);
	for (i = 0; i < w.a.iters; i++) {
		usleep(50);
		clock_gettime(CLOCK_MONOTONIC, &w.t0);
		__atomic_store_n(&flag, i + 1, __ATOMIC_SEQ_CST);
		wake_up(&q);
	}
	pthread_join(th, NULL);
	CHECK(w.a.worst_us < 50000);
	destroy_waitqueue_head(&q);
}

/* --- allocator semantics ------------------------------------------------ */

struct ctor_obj { int magic; char pad[100]; };
static int ctor_calls;
static void obj_ctor(void *p) { ((struct ctor_obj *)p)->magic = 0x5a5a; ctor_calls++; }

static void test_mem(void)
{
	struct kmem_cache *c = kmem_cache_create("ctor", sizeof(struct ctor_obj), 0,
						 SLAB_HWCACHE_ALIGN, obj_ctor);
	struct ctor_obj *o[8];
	int i;
	char *p;

	CHECK(c != NULL);
	/* Every object handed out has been constructed, exactly like a slab
	 * object (the ntfs inode cache relies on inode_init_once having run). */
	for (i = 0; i < 8; i++) {
		o[i] = kmem_cache_alloc(c, GFP_KERNEL);
		CHECK(o[i] && o[i]->magic == 0x5a5a);
		CHECK(((uintptr_t)o[i] & 63) == 0);	/* SLAB_HWCACHE_ALIGN */
		o[i]->magic = 0;			/* dirty it before free */
	}
	for (i = 0; i < 8; i++)
		kmem_cache_free(c, o[i]);
	o[0] = kmem_cache_alloc(c, GFP_KERNEL);
	CHECK(o[0]->magic == 0x5a5a);
	kmem_cache_free(c, o[0]);
	CHECK(ctor_calls == 9);
	kmem_cache_destroy(c);
	/* krealloc grows with __GFP_ZERO zeroing the tail, keeps contents. */
	p = kmalloc(16, GFP_KERNEL);
	memset(p, 'x', 16);
	p = krealloc(p, 4096, GFP_KERNEL | __GFP_ZERO);
	CHECK(p && p[0] == 'x' && p[15] == 'x' && p[16] == 0 && p[4095] == 0);
	kfree(p);
	/* Page allocations are page aligned and zeroed on request. */
	{
		unsigned long a = get_zeroed_page(GFP_KERNEL);
		CHECK(a && (a & (PAGE_SIZE - 1)) == 0 && ((char *)a)[PAGE_SIZE - 1] == 0);
		free_page(a);
		a = __get_free_pages(GFP_KERNEL, 2);
		CHECK(a && (a & (PAGE_SIZE - 1)) == 0);
		free_pages(a, 2);
	}
}

int main(void)
{
	make_image();
	test_rw();
	test_readonly_and_fd();
	test_rmw_race();
	test_work();
	test_wait();
	test_mem();
	unlink(path);
	pagecache_writeback_stop();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_bdev: ok\n");
	return 0;
}
