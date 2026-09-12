/* SPDX-License-Identifier: GPL-2.0 */
/*
 * User-space page cache for the ported NTFS driver.
 *
 * Implements everything declared in <linux/pagemap.h> with kernel
 * semantics. See docs/PORTING.md §3 (rules 2-4) for why this matters: every
 * MFT record, index block and bitmap the core touches goes through here.
 *
 * Structures
 *   Per address_space (in mapping->tree, struct pc_index): a chained hash
 *   table keyed by folio index (buckets grow at load factor 1), plus the
 *   LRU and dirty lists embedded in mapping. Folio->node is the hash chain
 *   link. A global registry of mappings feeds the write-back thread.
 *
 * Locking order (outer to inner)
 *   registry_lock  >  folio->lock  >  mapping->tree_lock  >  state_lock
 *   folio->lock is never acquired while holding tree_lock (trylock only).
 *   registry_lock is never held across I/O: the writer marks a mapping busy
 *   (pc_index->busy) instead, and address_space_destroy() waits for busy==0.
 *
 * References
 *   The hash table holds one reference on every inserted folio. Lookups
 *   hand out +1. folio_put() frees at zero, which can only happen after the
 *   folio was removed from the table (truncate/invalidate/reclaim/destroy).
 */
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/slab.h>

/* ------------------------------------------------------------------ */
/* Private state                                                       */

struct pc_index {
	struct folio **buckets;
	unsigned long nbuckets;		/* power of two */
	unsigned long count;
	unsigned long busy;		/* writer / sync passes in progress */
	struct list_head registry;	/* on g_registry */
	struct address_space *mapping;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(g_registry);

/* Waiters for folio state transitions (writeback end, busy==0). */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;

static atomic64_t g_nr_folios = ATOMIC64_INIT(0);

static pthread_t writer_thread;
static pthread_mutex_t writer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t writer_cond = PTHREAD_COND_INITIALIZER;
static int writer_state;	/* 0 stopped, 1 running, 2 stop requested */

#define WRITER_TICK_NS (200 * NSEC_PER_MSEC)

/* Not in the shared list.h; private to this file. */
#define pc_list_for_each_entry_safe_reverse(pos, n, head, member) \
	for (pos = list_last_entry(head, __typeof__(*pos), member), \
	     n = list_prev_entry(pos, member); \
	     &pos->member != (head); \
	     pos = n, n = list_prev_entry(n, member))

static inline u64 now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (u64)t.tv_sec * NSEC_PER_SEC + t.tv_nsec;
}

static inline struct pc_index *idx_of(struct address_space *m)
{
	return (struct pc_index *)m->tree;
}

static inline unsigned long hash_index(pgoff_t index, unsigned long nbuckets)
{
	u64 h = index * 0x9E3779B97F4A7C15ULL;
	return (unsigned long)(h >> 20) & (nbuckets - 1);
}

/* ------------------------------------------------------------------ */
/* Hash table (caller holds tree_lock)                                 */

static int idx_grow(struct pc_index *ix)
{
	unsigned long nb = ix->nbuckets * 2;
	struct folio **nbk = calloc(nb, sizeof(*nbk));
	unsigned long i;

	if (!nbk)
		return -ENOMEM;
	for (i = 0; i < ix->nbuckets; i++) {
		struct folio *f = ix->buckets[i];
		while (f) {
			struct folio *next = f->node;
			unsigned long b = hash_index(f->index, nb);
			f->node = nbk[b];
			nbk[b] = f;
			f = next;
		}
	}
	free(ix->buckets);
	ix->buckets = nbk;
	ix->nbuckets = nb;
	return 0;
}

static struct folio *idx_lookup(struct pc_index *ix, pgoff_t index)
{
	struct folio *f = ix->buckets[hash_index(index, ix->nbuckets)];
	while (f && f->index != index)
		f = f->node;
	return f;
}

static void idx_insert(struct pc_index *ix, struct folio *f)
{
	unsigned long b;

	if (ix->count >= ix->nbuckets)
		(void)idx_grow(ix);	/* on failure the chains just get longer */
	b = hash_index(f->index, ix->nbuckets);
	f->node = ix->buckets[b];
	ix->buckets[b] = f;
	ix->count++;
}

static void idx_remove(struct pc_index *ix, struct folio *f)
{
	struct folio **pp = &ix->buckets[hash_index(f->index, ix->nbuckets)];
	while (*pp && *pp != f)
		pp = (struct folio **)&(*pp)->node;
	if (*pp) {
		*pp = f->node;
		ix->count--;
	}
	f->node = NULL;
}

/* ------------------------------------------------------------------ */
/* Folio allocation                                                    */

static struct folio *folio_alloc_new(struct address_space *mapping, pgoff_t index)
{
	struct folio *f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	if (posix_memalign(&f->data, PAGE_SIZE, PAGE_SIZE) != 0) {
		free(f);
		return NULL;
	}
	atomic_set(&f->_refcount, 1);	/* the caller's reference */
	f->mapping = mapping;
	f->index = index;
	mutex_init(&f->lock);
	INIT_LIST_HEAD(&f->lru);
	INIT_LIST_HEAD(&f->dirty_list);
	atomic64_inc(&g_nr_folios);
	return f;
}

static void folio_free(struct folio *f)
{
	mutex_destroy(&f->lock);
	free(f->data);
	free(f);
	atomic64_dec(&g_nr_folios);
}

void folio_get(struct folio *folio)
{
	atomic_inc(&folio->_refcount);
}

void folio_put(struct folio *folio)
{
	if (atomic_dec_and_test(&folio->_refcount))
		folio_free(folio);
}

/* ------------------------------------------------------------------ */
/* Locking                                                             */

void folio_lock(struct folio *folio) { mutex_lock(&folio->lock); set_bit(PG_locked, &folio->flags); }
int folio_trylock(struct folio *folio)
{
	if (!mutex_trylock(&folio->lock))
		return 0;
	set_bit(PG_locked, &folio->flags);
	return 1;
}
void folio_unlock(struct folio *folio)
{
	clear_bit(PG_locked, &folio->flags);
	mutex_unlock(&folio->lock);
	pthread_mutex_lock(&state_lock);
	pthread_cond_broadcast(&state_cond);
	pthread_mutex_unlock(&state_lock);
}
void folio_wait_locked(struct folio *folio)
{
	pthread_mutex_lock(&state_lock);
	while (folio_test_locked(folio))
		pthread_cond_wait(&state_cond, &state_lock);
	pthread_mutex_unlock(&state_lock);
}

/* ------------------------------------------------------------------ */
/* State bits                                                          */

void folio_mark_uptodate(struct folio *folio) { set_bit(PG_uptodate, &folio->flags); }
void folio_clear_uptodate(struct folio *folio) { clear_bit(PG_uptodate, &folio->flags); }
void folio_set_error(struct folio *folio) { set_bit(PG_error, &folio->flags); }

/* Caller holds tree_lock. */
static bool __folio_set_dirty(struct address_space *m, struct folio *f)
{
	if (test_and_set_bit(PG_dirty, &f->flags))
		return false;
	f->dirtied_at_ns = now_ns();
	list_add_tail(&f->dirty_list, &m->dirty);
	m->nrdirty++;
	return true;
}

/* Caller holds tree_lock. */
static bool __folio_clear_dirty(struct address_space *m, struct folio *f)
{
	if (!test_and_clear_bit(PG_dirty, &f->flags))
		return false;
	list_del_init(&f->dirty_list);
	m->nrdirty--;
	return true;
}

bool folio_mark_dirty(struct folio *folio)
{
	struct address_space *m = folio->mapping;
	bool newly;

	if (!m)
		return false;		/* truncated away; nothing to write */
	spin_lock(&m->tree_lock);
	newly = __folio_set_dirty(m, folio);
	spin_unlock(&m->tree_lock);
	return newly;
}

void folio_clear_dirty(struct folio *folio)
{
	struct address_space *m = folio->mapping;
	if (!m) {
		clear_bit(PG_dirty, &folio->flags);
		return;
	}
	spin_lock(&m->tree_lock);
	__folio_clear_dirty(m, folio);
	spin_unlock(&m->tree_lock);
}

bool folio_clear_dirty_for_io(struct folio *folio)
{
	struct address_space *m = folio->mapping;
	bool was;
	if (!m)
		return test_and_clear_bit(PG_dirty, &folio->flags);
	spin_lock(&m->tree_lock);
	was = __folio_clear_dirty(m, folio);
	spin_unlock(&m->tree_lock);
	return was;
}

void folio_start_writeback(struct folio *folio) { set_bit(PG_writeback, &folio->flags); }
void folio_end_writeback(struct folio *folio)
{
	clear_bit(PG_writeback, &folio->flags);
	pthread_mutex_lock(&state_lock);
	pthread_cond_broadcast(&state_cond);
	pthread_mutex_unlock(&state_lock);
}
void folio_wait_writeback(struct folio *folio)
{
	pthread_mutex_lock(&state_lock);
	while (folio_test_writeback(folio))
		pthread_cond_wait(&state_cond, &state_lock);
	pthread_mutex_unlock(&state_lock);
}
void folio_redirty_for_writepage(void *wbc, struct folio *folio)
{
	(void)wbc;
	folio_mark_dirty(folio);
}

/* ------------------------------------------------------------------ */
/* Contents                                                            */

void folio_zero_segment(struct folio *folio, size_t start, size_t end)
{
	if (end > PAGE_SIZE) end = PAGE_SIZE;
	if (start < end) memset((char *)folio->data + start, 0, end - start);
}
void folio_zero_segments(struct folio *folio, size_t s1, size_t e1, size_t s2, size_t e2)
{
	folio_zero_segment(folio, s1, e1);
	folio_zero_segment(folio, s2, e2);
}
void folio_zero_range(struct folio *folio, size_t start, size_t length)
{
	folio_zero_segment(folio, start, start + length);
}
void folio_fill_tail(struct folio *folio, size_t offset, const char *from, size_t len)
{
	if (offset > PAGE_SIZE) offset = PAGE_SIZE;
	if (len > PAGE_SIZE - offset) len = PAGE_SIZE - offset;
	if (len) memcpy((char *)folio->data + offset, from, len);
	folio_zero_segment(folio, offset + len, PAGE_SIZE);
}
size_t memcpy_from_folio(char *to, struct folio *folio, size_t offset, size_t len)
{
	if (offset >= PAGE_SIZE) return 0;
	if (len > PAGE_SIZE - offset) len = PAGE_SIZE - offset;
	memcpy(to, (char *)folio->data + offset, len);
	return len;
}
size_t memcpy_to_folio(struct folio *folio, size_t offset, const char *from, size_t len)
{
	if (offset >= PAGE_SIZE) return 0;
	if (len > PAGE_SIZE - offset) len = PAGE_SIZE - offset;
	memcpy((char *)folio->data + offset, from, len);
	return len;
}

/* ------------------------------------------------------------------ */
/* Reclaim (caller holds tree_lock of @m)                              */

static void __reclaim_from(struct address_space *m, long want)
{
	struct folio *f, *tmp;

	pc_list_for_each_entry_safe_reverse(f, tmp, &m->lru, lru) {
		if (want <= 0)
			break;
		if (atomic_read(&f->_refcount) != 1 || folio_test_dirty(f) ||
		    folio_test_writeback(f))
			continue;
		if (!mutex_trylock(&f->lock))
			continue;
		/* Referenced by nobody but us; safe to drop. */
		idx_remove(idx_of(m), f);
		list_del_init(&f->lru);
		m->nrpages--;
		f->mapping = NULL;
		mutex_unlock(&f->lock);
		folio_put(f);		/* the table's reference; frees it */
		want--;
	}
}

static void maybe_reclaim(struct address_space *m)
{
	long over = (long)atomic64_read(&g_nr_folios) - (long)NTFS_PAGECACHE_MAX_FOLIOS;
	if (over > 0)
		__reclaim_from(m, over);
}

/* ------------------------------------------------------------------ */
/* Lookup / create                                                     */

struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index,
				  fgf_t fgp, gfp_t gfp)
{
	struct pc_index *ix = idx_of(mapping);
	struct folio *f, *nf = NULL;

	(void)gfp;
	if (!ix)
		return ERR_PTR(-EINVAL);
repeat:
	spin_lock(&mapping->tree_lock);
	f = idx_lookup(ix, index);
	if (f) {
		folio_get(f);
		if (fgp & FGP_ACCESSED)
			list_move(&f->lru, &mapping->lru);
		spin_unlock(&mapping->tree_lock);
		if (nf) {
			mutex_unlock(&nf->lock);
			folio_put(nf);
			nf = NULL;
		}
		if (fgp & FGP_LOCK) {
			if (fgp & FGP_NOWAIT) {
				if (!folio_trylock(f)) {
					folio_put(f);
					return ERR_PTR(-EAGAIN);
				}
			} else {
				folio_lock(f);
			}
			/* Truncated while we waited for the lock? */
			if (f->mapping != mapping || f->index != index) {
				folio_unlock(f);
				folio_put(f);
				goto repeat;
			}
		}
		return f;
	}
	if (!(fgp & FGP_CREAT)) {
		spin_unlock(&mapping->tree_lock);
		return ERR_PTR(-ENOENT);
	}
	if (!nf) {
		spin_unlock(&mapping->tree_lock);
		nf = folio_alloc_new(mapping, index);
		if (!nf)
			return ERR_PTR(-ENOMEM);
		/* Lock before insert so concurrent readers wait for our read. */
		mutex_lock(&nf->lock);
		set_bit(PG_locked, &nf->flags);
		goto repeat;
	}
	/* Insert: table reference + caller reference. */
	folio_get(nf);
	idx_insert(ix, nf);
	list_add(&nf->lru, &mapping->lru);
	mapping->nrpages++;
	maybe_reclaim(mapping);
	spin_unlock(&mapping->tree_lock);
	if (!(fgp & FGP_LOCK))
		folio_unlock(nf);
	return nf;
}

struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index)
{
	return __filemap_get_folio(mapping, index, FGP_ACCESSED, 0);
}

struct folio *filemap_lock_folio(struct address_space *mapping, pgoff_t index)
{
	return __filemap_get_folio(mapping, index, FGP_LOCK | FGP_ACCESSED, 0);
}

struct page *grab_cache_page_nowait(struct address_space *mapping, pgoff_t index)
{
	struct folio *f = __filemap_get_folio(mapping, index,
			FGP_LOCK | FGP_CREAT | FGP_NOWAIT | FGP_ACCESSED, 0);
	return IS_ERR(f) ? NULL : folio_page(f, 0);
}

static void __remove_locked(struct address_space *m, struct folio *f);
struct folio *read_mapping_folio(struct address_space *mapping, pgoff_t index,
				 struct file *file)
{
	struct folio *f;
	int err;

	(void)file;
	f = __filemap_get_folio(mapping, index, FGP_LOCK | FGP_CREAT | FGP_ACCESSED, 0);
	if (IS_ERR(f))
		return f;
	if (folio_test_uptodate(f)) {
		folio_unlock(f);
		return f;
	}
	if (!mapping->a_ops || !mapping->a_ops->read_folio) {
		__remove_locked(mapping, f);
		folio_unlock(f);
		folio_put(f);
		return ERR_PTR(-EIO);
	}
	clear_bit(PG_error, &f->flags);
	err = mapping->a_ops->read_folio(mapping, f);
	if (!err && !folio_test_uptodate(f))
		err = -EIO;
	if (err) {
		/* A folio that never became uptodate must not stay cached:
		 * the next lookup would hand out garbage as a hit. */
		folio_set_error(f);
		__remove_locked(mapping, f);
		folio_unlock(f);
		folio_put(f);
		return ERR_PTR(err);
	}
	folio_unlock(f);
	return f;
}

/* ------------------------------------------------------------------ */
/* Write-back                                                          */

/* Write one folio. Caller holds the folio lock and a reference. */
static int writeback_one(struct address_space *m, struct folio *f)
{
	int err;

	if (!folio_clear_dirty_for_io(f))
		return 0;
	if (f->mapping != m)		/* truncated under us */
		return 0;
	folio_start_writeback(f);
	err = m->a_ops && m->a_ops->write_folio ? m->a_ops->write_folio(m, f) : -EIO;
	if (err) {
		folio_set_error(f);
		spin_lock(&m->tree_lock);
		if (!m->wb_err)
			m->wb_err = err;
		spin_unlock(&m->tree_lock);
	}
	folio_end_writeback(f);
	return err;
}

/*
 * Collect referenced dirty folios of @m within [first, last] dirtied before
 * @older_than_ns (0 = all), sorted by index, into a malloc'ed array.
 */
static int cmp_folio_index(const void *a, const void *b)
{
	const struct folio *x = *(struct folio *const *)a, *y = *(struct folio *const *)b;
	return x->index < y->index ? -1 : x->index > y->index;
}

static struct folio **collect_dirty(struct address_space *m, pgoff_t first,
				    pgoff_t last, u64 older_than_ns, size_t *n_out)
{
	struct folio **arr = NULL, *f;
	size_t n = 0, cap = 0;

	spin_lock(&m->tree_lock);
	list_for_each_entry(f, &m->dirty, dirty_list) {
		if (older_than_ns && f->dirtied_at_ns > older_than_ns)
			break;		/* list is oldest first */
		if (f->index < first || f->index > last)
			continue;
		if (n == cap) {
			size_t ncap = cap ? cap * 2 : 64;
			struct folio **na = realloc(arr, ncap * sizeof(*na));
			if (!na)
				break;
			arr = na;
			cap = ncap;
		}
		folio_get(f);
		arr[n++] = f;
	}
	spin_unlock(&m->tree_lock);
	if (n > 1)
		qsort(arr, n, sizeof(*arr), cmp_folio_index);
	*n_out = n;
	return arr;
}

static int write_range(struct address_space *m, pgoff_t first, pgoff_t last,
		       u64 older_than_ns)
{
	size_t n, i;
	struct folio **arr = collect_dirty(m, first, last, older_than_ns, &n);
	int err = 0, e;

	for (i = 0; i < n; i++) {
		folio_lock(arr[i]);
		e = writeback_one(m, arr[i]);
		folio_unlock(arr[i]);
		folio_put(arr[i]);
		if (e && !err)
			err = e;
	}
	free(arr);
	return err;
}

/* Wait for in-flight write-back of folios in range (caller holds nothing). */
static void wait_range(struct address_space *m, pgoff_t first, pgoff_t last)
{
	struct pc_index *ix = idx_of(m);
	unsigned long b;
	bool again;

	do {
		struct folio *victim = NULL;
		again = false;
		spin_lock(&m->tree_lock);
		for (b = 0; b < ix->nbuckets && !victim; b++) {
			struct folio *f;
			for (f = ix->buckets[b]; f; f = f->node) {
				if (f->index >= first && f->index <= last &&
				    folio_test_writeback(f)) {
					folio_get(f);
					victim = f;
					break;
				}
			}
		}
		spin_unlock(&m->tree_lock);
		if (victim) {
			folio_wait_writeback(victim);
			folio_put(victim);
			again = true;
		}
	} while (again);
}

static int report_wb_err(struct address_space *m)
{
	int err;
	spin_lock(&m->tree_lock);
	err = m->wb_err;
	m->wb_err = 0;
	spin_unlock(&m->tree_lock);
	return err;
}

static inline pgoff_t byte_to_index(loff_t b) { return (pgoff_t)b >> PAGE_SHIFT; }

int filemap_fdatawrite_range(struct address_space *mapping, loff_t start, loff_t end)
{
	if (!idx_of(mapping))
		return 0;
	return write_range(mapping, byte_to_index(start),
			   end < 0 ? (pgoff_t)-1 : byte_to_index(end), 0);
}

int filemap_fdatawait_range(struct address_space *mapping, loff_t start, loff_t end)
{
	if (!idx_of(mapping))
		return 0;
	wait_range(mapping, byte_to_index(start), end < 0 ? (pgoff_t)-1 : byte_to_index(end));
	return report_wb_err(mapping);
}

int filemap_write_and_wait_range(struct address_space *mapping, loff_t lstart, loff_t lend)
{
	int err, err2;
	if (!idx_of(mapping))
		return 0;
	err = filemap_fdatawrite_range(mapping, lstart, lend);
	err2 = filemap_fdatawait_range(mapping, lstart, lend);
	return err ? err : err2;
}

int filemap_write_and_wait(struct address_space *mapping)
{
	return filemap_write_and_wait_range(mapping, 0, -1);
}

int filemap_flush(struct address_space *mapping)
{
	return filemap_fdatawrite_range(mapping, 0, -1);
}

/* ------------------------------------------------------------------ */
/* Truncate / invalidate                                               */

/*
 * Remove @f from the table. Caller holds the folio lock and a reference and
 * the tree_lock is NOT held. Drops the table's reference.
 */
static void __remove_locked(struct address_space *m, struct folio *f)
{
	spin_lock(&m->tree_lock);
	if (f->mapping != m) {
		spin_unlock(&m->tree_lock);
		return;
	}
	__folio_clear_dirty(m, f);
	idx_remove(idx_of(m), f);
	list_del_init(&f->lru);
	m->nrpages--;
	f->mapping = NULL;
	spin_unlock(&m->tree_lock);
	folio_put(f);
}

/* Snapshot of every folio in [first, last], each referenced. */
static struct folio **collect_range(struct address_space *m, pgoff_t first,
				    pgoff_t last, size_t *n_out)
{
	struct pc_index *ix = idx_of(m);
	struct folio **arr = NULL;
	size_t n = 0, cap = 0;
	unsigned long b;

	spin_lock(&m->tree_lock);
	for (b = 0; b < ix->nbuckets; b++) {
		struct folio *f;
		for (f = ix->buckets[b]; f; f = f->node) {
			if (f->index < first || f->index > last)
				continue;
			if (n == cap) {
				size_t ncap = cap ? cap * 2 : 64;
				struct folio **na = realloc(arr, ncap * sizeof(*na));
				if (!na)
					goto out;
				arr = na;
				cap = ncap;
			}
			folio_get(f);
			arr[n++] = f;
		}
	}
out:
	spin_unlock(&m->tree_lock);
	if (n > 1)
		qsort(arr, n, sizeof(*arr), cmp_folio_index);
	*n_out = n;
	return arr;
}

/*
 * Drop folios in range. mode: 0 = truncate (wait for writeback, drop even
 * dirty/referenced); 1 = invalidate clean unreferenced only; 2 = invalidate
 * all, report -EBUSY for referenced ones.
 */
static long drop_range(struct address_space *m, pgoff_t first, pgoff_t last, int mode)
{
	size_t n, i;
	struct folio **arr;
	long dropped = 0;
	int err = 0;

	if (!idx_of(m))
		return 0;
	arr = collect_range(m, first, last, &n);
	for (i = 0; i < n; i++) {
		struct folio *f = arr[i];
		if (mode == 1) {
			if (!folio_trylock(f)) {
				folio_put(f);
				continue;
			}
			/* Our ref + table ref == 2 means nobody else. */
			if (atomic_read(&f->_refcount) != 2 || folio_test_dirty(f) ||
			    folio_test_writeback(f)) {
				folio_unlock(f);
				folio_put(f);
				continue;
			}
		} else {
			folio_lock(f);
			folio_wait_writeback(f);
			if (mode == 2 && atomic_read(&f->_refcount) != 2 && !err)
				err = -EBUSY;
		}
		if (f->mapping == m) {
			__remove_locked(m, f);
			dropped++;
		}
		folio_unlock(f);
		folio_put(f);
	}
	free(arr);
	return mode == 2 && err ? err : dropped;
}

void truncate_inode_pages_range(struct address_space *mapping, loff_t lstart, loff_t lend)
{
	pgoff_t first = byte_to_index(lstart + PAGE_SIZE - 1);
	pgoff_t last = lend < 0 ? (pgoff_t)-1 : byte_to_index(lend);

	if (!idx_of(mapping))
		return;
	/* Partial first folio: zero its tail. */
	if (lstart & ~PAGE_MASK) {
		struct folio *f = __filemap_get_folio(mapping, byte_to_index(lstart), FGP_LOCK, 0);
		if (!IS_ERR(f)) {
			folio_zero_segment(f, lstart & ~PAGE_MASK, PAGE_SIZE);
			folio_unlock(f);
			folio_put(f);
		}
	}
	if (first <= last)
		drop_range(mapping, first, last, 0);
}

void truncate_inode_pages(struct address_space *mapping, loff_t lstart)
{
	truncate_inode_pages_range(mapping, lstart, -1);
}

void truncate_inode_pages_final(struct address_space *mapping)
{
	truncate_inode_pages(mapping, 0);
}

void truncate_pagecache(struct inode *inode, loff_t newsize)
{
	truncate_inode_pages(inode->i_mapping, newsize);
}

void truncate_setsize(struct inode *inode, loff_t newsize)
{
	i_size_write(inode, newsize);
	truncate_pagecache(inode, newsize);
}

unsigned long invalidate_mapping_pages(struct address_space *mapping, pgoff_t start, pgoff_t end)
{
	return (unsigned long)drop_range(mapping, start, end, 1);
}

int invalidate_inode_pages2_range(struct address_space *mapping, pgoff_t start, pgoff_t end)
{
	long r = drop_range(mapping, start, end, 2);
	return r < 0 ? (int)r : 0;
}

/* ------------------------------------------------------------------ */
/* Writer thread                                                       */

static void mapping_busy_inc(struct pc_index *ix) { ix->busy++; }	/* registry_lock held */
static void mapping_busy_dec(struct pc_index *ix)
{
	pthread_mutex_lock(&state_lock);
	ix->busy--;
	pthread_cond_broadcast(&state_cond);
	pthread_mutex_unlock(&state_lock);
}

/*
 * Run @fn(mapping) for every registered mapping (optionally only those of
 * @sb) without holding registry_lock across the call.
 */
static int for_each_mapping(struct super_block *sb, int (*fn)(struct address_space *, void *),
			    void *arg)
{
	struct pc_index cursor = { .mapping = NULL };	/* skipped by others */
	struct list_head *pos;
	int err = 0;

	pthread_mutex_lock(&registry_lock);
	for (pos = g_registry.next; pos != &g_registry; pos = pos->next) {
		struct pc_index *ix = list_entry(pos, struct pc_index, registry);
		struct address_space *m = ix->mapping;
		int e;
		if (!m)
			continue;	/* another iterator's cursor */
		if (sb && (!m->host || m->host->i_sb != sb))
			continue;
		/* Park a cursor after ix so a concurrent destroy of ix cannot
		 * strand us; busy>0 keeps ix's memory alive meanwhile. */
		list_add(&cursor.registry, pos);
		mapping_busy_inc(ix);
		pthread_mutex_unlock(&registry_lock);
		e = fn(m, arg);
		if (e && !err)
			err = e;
		pthread_mutex_lock(&registry_lock);
		mapping_busy_dec(ix);
		pos = cursor.registry.prev;	/* loop advances to cursor.next */
		list_del(&cursor.registry);
	}
	pthread_mutex_unlock(&registry_lock);
	return err;
}

static int writer_pass_fn(struct address_space *m, void *arg)
{
	u64 cutoff = *(u64 *)arg;
	int err = write_range(m, 0, (pgoff_t)-1, cutoff);
	long over;

	/* Global reclaim when over the cap. */
	over = (long)atomic64_read(&g_nr_folios) - (long)NTFS_PAGECACHE_MAX_FOLIOS;
	if (over > 0) {
		spin_lock(&m->tree_lock);
		__reclaim_from(m, over);
		spin_unlock(&m->tree_lock);
	}
	return err;
}

static int sync_fn(struct address_space *m, void *arg)
{
	(void)arg;
	return filemap_write_and_wait(m);
}

static void *writer_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&writer_lock);
	while (writer_state == 1) {
		struct timespec ts;
		u64 deadline = now_ns() + WRITER_TICK_NS, cutoff;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += WRITER_TICK_NS;
		while (ts.tv_nsec >= NSEC_PER_SEC) { ts.tv_nsec -= NSEC_PER_SEC; ts.tv_sec++; }
		while (writer_state == 1 && now_ns() < deadline)
			pthread_cond_timedwait(&writer_cond, &writer_lock, &ts);
		if (writer_state != 1)
			break;
		pthread_mutex_unlock(&writer_lock);
		cutoff = now_ns() - (u64)NTFS_WRITEBACK_INTERVAL_MS * NSEC_PER_MSEC;
		for_each_mapping(NULL, writer_pass_fn, &cutoff);
		pthread_mutex_lock(&writer_lock);
	}
	writer_state = 0;
	pthread_cond_broadcast(&writer_cond);
	pthread_mutex_unlock(&writer_lock);
	return NULL;
}

int pagecache_writeback_start(void)
{
	int err = 0;
	pthread_mutex_lock(&writer_lock);
	if (writer_state == 0) {
		writer_state = 1;
		err = pthread_create(&writer_thread, NULL, writer_main, NULL);
		if (err) {
			writer_state = 0;
			err = -err;
		} else {
			pthread_detach(writer_thread);
		}
	}
	pthread_mutex_unlock(&writer_lock);
	return err;
}

void pagecache_writeback_stop(void)
{
	pthread_mutex_lock(&writer_lock);
	if (writer_state == 1) {
		writer_state = 2;
		pthread_cond_broadcast(&writer_cond);
		while (writer_state != 0)
			pthread_cond_wait(&writer_cond, &writer_lock);
	}
	pthread_mutex_unlock(&writer_lock);
}

int pagecache_sync_sb(struct super_block *sb)
{
	return for_each_mapping(sb, sync_fn, NULL);
}

/* ------------------------------------------------------------------ */
/* Mapping lifecycle                                                   */

void address_space_init(struct address_space *mapping, struct inode *host,
			const struct address_space_operations *a_ops)
{
	struct pc_index *ix = calloc(1, sizeof(*ix));

	memset(mapping, 0, sizeof(*mapping));
	mapping->host = host;
	mapping->a_ops = a_ops;
	mapping->gfp_mask = GFP_HIGHUSER;
	init_rwsem(&mapping->invalidate_lock);
	INIT_LIST_HEAD(&mapping->private_list);
	spin_lock_init(&mapping->tree_lock);
	INIT_LIST_HEAD(&mapping->lru);
	INIT_LIST_HEAD(&mapping->dirty);
	if (!ix)
		return;			/* lookups will fail with -EINVAL */
	ix->nbuckets = 16;
	ix->buckets = calloc(ix->nbuckets, sizeof(*ix->buckets));
	if (!ix->buckets) {
		free(ix);
		return;
	}
	ix->mapping = mapping;
	mapping->tree = ix;
	pthread_mutex_lock(&registry_lock);
	list_add_tail(&ix->registry, &g_registry);
	pthread_mutex_unlock(&registry_lock);
	pagecache_writeback_start();
}

void address_space_destroy(struct address_space *mapping, bool discard)
{
	struct pc_index *ix = idx_of(mapping);
	unsigned long b;

	if (!ix)
		return;
	/* Unregister, then wait for any pass that still holds us busy. */
	pthread_mutex_lock(&registry_lock);
	list_del_init(&ix->registry);
	pthread_mutex_unlock(&registry_lock);
	pthread_mutex_lock(&state_lock);
	while (ix->busy)
		pthread_cond_wait(&state_cond, &state_lock);
	pthread_mutex_unlock(&state_lock);

	if (!discard)
		filemap_write_and_wait(mapping);
	/* Drop everything, dirty or not. */
	drop_range(mapping, 0, (pgoff_t)-1, 0);

	spin_lock(&mapping->tree_lock);
	for (b = 0; b < ix->nbuckets; b++)
		WARN_ON(ix->buckets[b] != NULL);
	mapping->tree = NULL;
	spin_unlock(&mapping->tree_lock);
	free(ix->buckets);
	free(ix);
	destroy_rwsem(&mapping->invalidate_lock);
}
