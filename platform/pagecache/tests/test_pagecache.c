/* SPDX-License-Identifier: GPL-2.0 */
/* Functional tests for the user-space page cache. */
#include <unistd.h>
#include "fake_disk.h"

static void test_read_hit_miss(struct inode *in)
{
	struct address_space *m = in->i_mapping;
	s64 r0 = atomic64_read(&fake_reads);
	struct folio *f = read_mapping_folio(m, 5, NULL);
	CHECK(!IS_ERR(f));
	CHECK(folio_test_uptodate(f));
	CHECK(!folio_test_locked(f));
	CHECK(memcmp(f->data, fake_addr(m, 5), PAGE_SIZE) == 0);
	CHECK(atomic64_read(&fake_reads) == r0 + 1);
	struct folio *g = read_mapping_folio(m, 5, NULL);
	CHECK(g == f);
	CHECK(atomic64_read(&fake_reads) == r0 + 1);	/* hit: no second read */
	CHECK(atomic_read(&f->_refcount) == 3);		/* table + 2 callers */
	folio_put(g);
	folio_put(f);
	CHECK(m->nrpages == 1);
	/* Missing without CREAT → -ENOENT. */
	CHECK(PTR_ERR(filemap_get_folio(m, 77)) == -ENOENT);
	/* Out of range index → read error propagates and nothing is cached. */
	CHECK(PTR_ERR(read_mapping_folio(m, REGION_PAGES + 1, NULL)) == -EIO);
	CHECK(m->nrpages == 1);
}

static void test_dirty_background(struct inode *in)
{
	struct address_space *m = in->i_mapping;
	struct folio *f = read_mapping_folio(m, 10, NULL);
	s64 w0 = atomic64_read(&fake_writes);
	CHECK(!IS_ERR(f));
	folio_lock(f);
	memset(f->data, 0xAB, PAGE_SIZE);
	CHECK(folio_mark_dirty(f) == true);
	CHECK(folio_mark_dirty(f) == false);
	CHECK(m->nrdirty == 1);
	folio_unlock(f);
	/* Background writer: NTFS_WRITEBACK_INTERVAL_MS + tick. */
	for (int i = 0; i < 40 && atomic64_read(&fake_writes) == w0; i++)
		usleep(50 * 1000);
	CHECK(atomic64_read(&fake_writes) == w0 + 1);
	CHECK(!folio_test_dirty(f));
	CHECK(m->nrdirty == 0);
	CHECK(fake_addr(m, 10)[100] == 0xAB);
	folio_put(f);
}

static void test_sync_and_ordering(struct inode *in)
{
	struct address_space *m = in->i_mapping;
	pgoff_t order[] = { 30, 20, 40, 25 };
	struct folio *fs[4];
	s64 w0 = atomic64_read(&fake_writes);
	for (int i = 0; i < 4; i++) {
		fs[i] = read_mapping_folio(m, order[i], NULL);
		CHECK(!IS_ERR(fs[i]));
		folio_lock(fs[i]);
		memset(fs[i]->data, (int)order[i], PAGE_SIZE);
		folio_mark_dirty(fs[i]);
		folio_unlock(fs[i]);
	}
	/* Dirty list is oldest first. */
	struct folio *pos; int k = 0;
	list_for_each_entry(pos, &m->dirty, dirty_list) CHECK(pos->index == order[k++]);
	CHECK(k == 4);
	/* Range sync writes only [20..30] (3 folios). */
	CHECK(filemap_write_and_wait_range(m, 20 * PAGE_SIZE, 30 * PAGE_SIZE + PAGE_SIZE - 1) == 0);
	CHECK(atomic64_read(&fake_writes) == w0 + 3);
	CHECK(m->nrdirty == 1);
	CHECK(folio_test_dirty(fs[2]));
	CHECK(filemap_write_and_wait(m) == 0);
	CHECK(atomic64_read(&fake_writes) == w0 + 4);
	CHECK(m->nrdirty == 0);
	for (int i = 0; i < 4; i++) {
		CHECK(fake_addr(m, order[i])[7] == (unsigned char)order[i]);
		folio_put(fs[i]);
	}
}

static void test_write_error(struct inode *in)
{
	struct address_space *m = in->i_mapping;
	struct folio *f = read_mapping_folio(m, 50, NULL);
	CHECK(!IS_ERR(f));
	folio_lock(f); folio_mark_dirty(f); folio_unlock(f);
	__atomic_store_n(&fake_fail_writes, 1, __ATOMIC_SEQ_CST);
	CHECK(filemap_write_and_wait(m) == -EIO);
	CHECK(folio_test_error(f));
	CHECK(filemap_write_and_wait(m) == 0);	/* error reported once */
	__atomic_store_n(&fake_fail_writes, 0, __ATOMIC_SEQ_CST);
	folio_put(f);
}

static void test_truncate(struct inode *in)
{
	struct address_space *m = in->i_mapping;
	struct folio *a = read_mapping_folio(m, 100, NULL);
	struct folio *b = read_mapping_folio(m, 101, NULL);
	struct folio *c = read_mapping_folio(m, 102, NULL);
	CHECK(!IS_ERR(a) && !IS_ERR(b) && !IS_ERR(c));
	folio_lock(b); memset(b->data, 0x55, PAGE_SIZE); folio_mark_dirty(b); folio_unlock(b);
	unsigned long before = m->nrpages;
	/* Truncate in the middle of folio 100: its tail is zeroed, 101+ dropped. */
	truncate_setsize(in, 100 * PAGE_SIZE + 123);
	CHECK(in->i_size == (loff_t)(100 * PAGE_SIZE + 123));
	CHECK(m->nrpages == before - 2);
	CHECK(a->mapping == m);
	CHECK(a->data != NULL);
	for (size_t i = 123; i < PAGE_SIZE; i++) CHECK(((unsigned char *)a->data)[i] == 0);
	CHECK(b->mapping == NULL);		/* dropped, held only by us */
	CHECK(!folio_test_dirty(b));
	CHECK(m->nrdirty == 0);
	CHECK(atomic_read(&b->_refcount) == 1);
	folio_put(b);
	folio_put(c);
	folio_put(a);
	/* Re-reading 101 comes from disk again. */
	s64 r0 = atomic64_read(&fake_reads);
	struct folio *b2 = read_mapping_folio(m, 101, NULL);
	CHECK(!IS_ERR(b2) && atomic64_read(&fake_reads) == r0 + 1);
	folio_put(b2);
	in->i_size = REGION_SIZE;
}

static void test_invalidate(struct inode *in)
{
	struct address_space *m = in->i_mapping;
	struct folio *held = read_mapping_folio(m, 200, NULL);
	struct folio *dirty = read_mapping_folio(m, 201, NULL);
	struct folio *clean = read_mapping_folio(m, 202, NULL);
	CHECK(!IS_ERR(held) && !IS_ERR(dirty) && !IS_ERR(clean));
	folio_lock(dirty); folio_mark_dirty(dirty); folio_unlock(dirty);
	folio_put(dirty);
	folio_put(clean);
	/* invalidate_mapping_pages drops only clean+unreferenced: just 202. */
	CHECK(invalidate_mapping_pages(m, 200, 202) == 1);
	CHECK(PTR_ERR(filemap_get_folio(m, 202)) == -ENOENT);
	CHECK(!IS_ERR(filemap_get_folio(m, 201)));
	folio_put(filemap_get_folio(m, 201));
	/* invalidate2 drops everything but reports the referenced one. */
	CHECK(invalidate_inode_pages2_range(m, 200, 202) == -EBUSY);
	CHECK(PTR_ERR(filemap_get_folio(m, 201)) == -ENOENT);
	CHECK(held->mapping == NULL);
	CHECK(m->nrdirty == 0);
	folio_put(held);
}

static void test_fgp_flags(struct inode *in)
{
	struct address_space *m = in->i_mapping;
	struct folio *f = __filemap_get_folio(m, 300, FGP_LOCK | FGP_CREAT, 0);
	CHECK(!IS_ERR(f));
	CHECK(folio_test_locked(f));
	CHECK(!folio_test_uptodate(f));
	/* NOWAIT on a locked folio fails with -EAGAIN. */
	CHECK(PTR_ERR(__filemap_get_folio(m, 300, FGP_LOCK | FGP_NOWAIT, 0)) == -EAGAIN);
	CHECK(grab_cache_page_nowait(m, 300) == NULL);
	folio_unlock(f);
	struct page *p = grab_cache_page_nowait(m, 300);
	CHECK(p == folio_page(f, 0));
	unlock_page(p);
	put_page(p);
	/* read_mapping_folio on a not-uptodate cached folio issues the read. */
	s64 r0 = atomic64_read(&fake_reads);
	struct folio *g = read_mapping_folio(m, 300, NULL);
	CHECK(g == f && folio_test_uptodate(g) && atomic64_read(&fake_reads) == r0 + 1);
	folio_put(g);
	folio_put(f);
	/* Contents helpers. */
	f = read_mapping_folio(m, 301, NULL);
	char buf[64];
	folio_fill_tail(f, 10, "hello", 5);
	CHECK(memcpy_from_folio(buf, f, 10, 5) == 5 && memcmp(buf, "hello", 5) == 0);
	CHECK(((char *)f->data)[PAGE_SIZE - 1] == 0);
	folio_zero_segments(f, 0, 4, 12, 16);
	CHECK(((char *)f->data)[13] == 0 && ((char *)f->data)[11] == 'e');
	CHECK(memcpy_to_folio(f, PAGE_SIZE - 2, "abcd", 4) == 2);
	CHECK(memcpy_from_folio(buf, f, PAGE_SIZE, 4) == 0);
	folio_put(f);
}

static void test_reclaim(void)
{
	/* Use a dedicated inode; fill beyond the cap. The cap is large by
	 * default (64k folios), so this test only runs when built with a small
	 * NTFS_PAGECACHE_MAX_FOLIOS; otherwise it just checks the counters. */
	struct inode in;
	fake_inode_init(&in, 3);
	struct address_space *m = in.i_mapping;
	struct folio *pin = read_mapping_folio(m, 0, NULL);
	struct folio *dirty = read_mapping_folio(m, 1, NULL);
	folio_lock(dirty); folio_mark_dirty(dirty); folio_unlock(dirty);
	folio_put(dirty);
	unsigned long n = NTFS_PAGECACHE_MAX_FOLIOS + 64;
	if (n > REGION_PAGES - 2) n = REGION_PAGES - 2;
	for (unsigned long i = 2; i < n; i++) {
		struct folio *f = read_mapping_folio(m, i, NULL);
		CHECK(!IS_ERR(f));
		folio_put(f);
	}
	if (NTFS_PAGECACHE_MAX_FOLIOS < REGION_PAGES) {
		CHECK(m->nrpages <= NTFS_PAGECACHE_MAX_FOLIOS + 8);
		CHECK(pin->mapping == m);			/* referenced: kept */
		CHECK(!IS_ERR(filemap_get_folio(m, 1)));	/* dirty: kept */
		folio_put(filemap_get_folio(m, 1));
	}
	folio_put(pin);
	address_space_destroy(m, false);
	CHECK(fake_addr(m, 1) != NULL);
}

static void test_destroy(void)
{
	struct inode a, b;
	fake_inode_init(&a, 1);
	fake_inode_init(&b, 2);
	struct folio *fa = read_mapping_folio(a.i_mapping, 9, NULL);
	struct folio *fb = read_mapping_folio(b.i_mapping, 9, NULL);
	folio_lock(fa); memset(fa->data, 0x11, PAGE_SIZE); folio_mark_dirty(fa); folio_unlock(fa);
	folio_lock(fb); memset(fb->data, 0x22, PAGE_SIZE); folio_mark_dirty(fb); folio_unlock(fb);
	unsigned char before_b = fake_addr(b.i_mapping, 9)[0];
	folio_put(fa); folio_put(fb);
	address_space_destroy(a.i_mapping, false);	/* flushes */
	address_space_destroy(b.i_mapping, true);	/* discards */
	CHECK(fake_disk[(size_t)1 * REGION_SIZE + 9 * PAGE_SIZE] == 0x11);
	CHECK(fake_disk[(size_t)2 * REGION_SIZE + 9 * PAGE_SIZE] == before_b);
	/* pagecache_sync_sb on an sb with no mappings is fine. */
	CHECK(pagecache_sync_sb(&fake_sb) == 0);
}

int main(void)
{
	struct inode in;
	fake_disk_init();
	fake_inode_init(&in, 0);
	test_read_hit_miss(&in);
	test_dirty_background(&in);
	test_sync_and_ordering(&in);
	test_write_error(&in);
	test_truncate(&in);
	test_invalidate(&in);
	test_fgp_flags(&in);
	/* sync_sb flushes this inode's dirty folios through its sb. */
	{
		struct folio *f = read_mapping_folio(in.i_mapping, 400, NULL);
		folio_lock(f); folio_mark_dirty(f); folio_unlock(f); folio_put(f);
		CHECK(pagecache_sync_sb(&fake_sb) == 0);
		CHECK(in.i_mapping->nrdirty == 0);
	}
	address_space_destroy(in.i_mapping, false);
	test_reclaim();
	test_destroy();
	pagecache_writeback_stop();
	printf("pagecache: all tests passed (reads=%lld writes=%lld)\n",
	       (long long)atomic64_read(&fake_reads), (long long)atomic64_read(&fake_writes));
	free(fake_disk);
	return 0;
}
