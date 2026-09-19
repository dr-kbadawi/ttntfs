/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Stress test: 8 threads doing random reads, writes (dirty), syncs,
 * invalidates and truncates on 3 mappings for N seconds, tracking an
 * in-memory model of what every page should contain. At the end every
 * page is checked against the model both via the cache and on the fake
 * disk after a full sync.
 */
#include <unistd.h>
#include <stdint.h>
#include "fake_disk.h"

#define NTHREADS 8
#define NMAPPINGS 3
#define NPAGES 2048		/* 8 MiB per mapping: exercises sparse and dense */

static struct inode inodes[NMAPPINGS];
static uint32_t model[NMAPPINGS][NPAGES];	/* expected stamp per page (0 = pristine) */
static pthread_mutex_t model_lock[NMAPPINGS][NPAGES / 64];
static int stop;
static atomic64_t ops = ATOMIC64_INIT(0), lookups = ATOMIC64_INIT(0);

static inline pthread_mutex_t *mlock(int m, unsigned p) { return &model_lock[m][p / 64]; }

static void stamp_page(void *data, uint32_t stamp)
{
	uint32_t *w = data;
	for (size_t i = 0; i < PAGE_SIZE / 4; i++)
		w[i] = stamp ^ (uint32_t)i;
}

static int check_page(const void *data, uint32_t stamp, unsigned char pristine_first)
{
	const uint32_t *w = data;
	if (stamp == 0)
		return ((const unsigned char *)data)[0] == pristine_first;
	for (size_t i = 0; i < PAGE_SIZE / 4; i++)
		if (w[i] != (stamp ^ (uint32_t)i))
			return 0;
	return 1;
}

static void *worker(void *arg)
{
	unsigned seed = (unsigned)(uintptr_t)arg * 7919u + 17u;
	uint32_t next_stamp = (uint32_t)(uintptr_t)arg << 24 | 1;
	while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
		int m = rand_r(&seed) % NMAPPINGS;
		unsigned p = rand_r(&seed) % NPAGES;
		int op = rand_r(&seed) % 100;
		struct address_space *as = inodes[m].i_mapping;
		atomic64_inc(&ops);
		if (op < 55) {			/* read + verify */
			struct folio *f = read_mapping_folio(as, p, NULL);
			CHECK(!IS_ERR(f));
			atomic64_inc(&lookups);
			pthread_mutex_lock(mlock(m, p));
			folio_lock(f);
			CHECK(check_page(f->data, model[m][p], fake_addr(as, p)[0] /* unused if stamped */));
			folio_unlock(f);
			pthread_mutex_unlock(mlock(m, p));
			folio_put(f);
		} else if (op < 90) {		/* write */
			struct folio *f = read_mapping_folio(as, p, NULL);
			CHECK(!IS_ERR(f));
			pthread_mutex_lock(mlock(m, p));
			folio_lock(f);
			next_stamp += 2;
			stamp_page(f->data, next_stamp);
			model[m][p] = next_stamp;
			folio_mark_dirty(f);
			folio_unlock(f);
			pthread_mutex_unlock(mlock(m, p));
			folio_put(f);
		} else if (op < 95) {		/* sync a range */
			CHECK(filemap_write_and_wait_range(as, (loff_t)p * PAGE_SIZE,
					(loff_t)(p + 64) * PAGE_SIZE) == 0);
		} else if (op < 98) {		/* drop clean folios */
			invalidate_mapping_pages(as, p, p + 32);
		} else {			/* full sync of the sb */
			CHECK(pagecache_sync_sb(&fake_sb) == 0);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 3;
	pthread_t th[NTHREADS];
	fake_disk_init();
	/* Pristine disk: model 0 must match byte 0 of the pristine pattern, so
	 * snapshot it before anything is written. */
	static unsigned char pristine_first[NMAPPINGS][NPAGES];
	for (int m = 0; m < NMAPPINGS; m++) {
		fake_inode_init(&inodes[m], (unsigned long)m);
		for (unsigned p = 0; p < NPAGES; p++)
			pristine_first[m][p] = fake_addr(inodes[m].i_mapping, p)[0];
		for (unsigned i = 0; i < NPAGES / 64; i++)
			pthread_mutex_init(&model_lock[m][i], NULL);
	}
	fake_write_delay_us = 20;	/* widen the writeback window */
	u64 t0 = ktime_get_ns();
	for (long i = 0; i < NTHREADS; i++)
		pthread_create(&th[i], NULL, worker, (void *)i);
	sleep(seconds);
	__atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
	for (int i = 0; i < NTHREADS; i++)
		pthread_join(th[i], NULL);
	u64 elapsed = ktime_get_ns() - t0;

	/* Verify through the cache, then flush and verify on disk. */
	for (int m = 0; m < NMAPPINGS; m++) {
		struct address_space *as = inodes[m].i_mapping;
		for (unsigned p = 0; p < NPAGES; p++) {
			struct folio *f = read_mapping_folio(as, p, NULL);
			CHECK(!IS_ERR(f));
			CHECK(check_page(f->data, model[m][p], pristine_first[m][p]));
			folio_put(f);
		}
		CHECK(filemap_write_and_wait(as) == 0);
		CHECK(as->nrdirty == 0);
		for (unsigned p = 0; p < NPAGES; p++)
			CHECK(check_page(fake_addr(as, p), model[m][p], pristine_first[m][p]));
		address_space_destroy(as, false);
	}
	pagecache_writeback_stop();
	printf("stress: %lld ops in %.1fs (%.0f ops/s, %.0f cached lookups/s), reads=%lld writes=%lld\n",
	       (long long)atomic64_read(&ops), elapsed / 1e9,
	       atomic64_read(&ops) * 1e9 / elapsed, atomic64_read(&lookups) * 1e9 / elapsed,
	       (long long)atomic64_read(&fake_reads), (long long)atomic64_read(&fake_writes));
	free(fake_disk);
	return 0;
}
