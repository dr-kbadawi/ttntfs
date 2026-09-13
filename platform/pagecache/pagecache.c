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
 *
 * Write-back state machine (platform review)
 *   A folio goes dirty -> (writeback && !dirty) -> clean under its lock.
 *   PG_writeback is set *before* PG_dirty is cleared so a concurrent
 *   filemap_write_and_wait_range(), which collects dirty folios and then
 *   waits for PG_writeback, can never observe the folio as neither: it
 *   either locks it (and finds it clean, written by us) or waits for it.
 *   A backend that re-dirties the folio (folio_redirty_for_writepage, used
 *   when a trylock fails) makes synchronous flushes retry a bounded number
 *   of times; the background writer simply picks it up next tick.
 */
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

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
	struct list_head dirty_registry;	/* on g_dirty, when it may be dirty */
	struct address_space *mapping;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(g_registry);
/*
 * Mappings that may hold dirty folios, so a sync does not have to walk every
 * mapping on the volume. A sync used to cost O(all cached mappings): with 8000
 * files cached and FSKit asking for a sync roughly every second operation, that
 * made each unlink O(directory size) and deleting a directory quadratic.
 *
 * The invariant is one-sided on purpose: every mapping with a dirty folio is on
 * this list, but a mapping on it may already be clean. Over-inclusion only
 * costs a wasted visit; under-inclusion would lose data. Entries are added on
 * the 0 -> 1 dirty transition and removed only by a sync that has just waited
 * for the mapping's writeback, which is the one moment "clean" also means
 * "nothing in flight".
 */
static LIST_HEAD(g_dirty);

/* Waiters for folio state transitions (unlock, writeback end, busy==0).
 * state_waiters is bumped under state_lock before the condition is checked
 * and read (seq_cst) by the signaller after it changed the state, so the
 * broadcast can be skipped when nobody waits without losing a wake-up. */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;
static int state_waiters;

static inline void state_changed(void)
{
	if (__atomic_load_n(&state_waiters, __ATOMIC_SEQ_CST)) {
		pthread_mutex_lock(&state_lock);
		pthread_cond_broadcast(&state_cond);
		pthread_mutex_unlock(&state_lock);
	}
}
/* Block (state_lock held) until @cond; the caller loops on it. */
#define state_wait_while(cond) do { \
	pthread_mutex_lock(&state_lock); \
	__atomic_add_fetch(&state_waiters, 1, __ATOMIC_SEQ_CST); \
	while (cond) \
		pthread_cond_wait(&state_cond, &state_lock); \
	__atomic_sub_fetch(&state_waiters, 1, __ATOMIC_SEQ_CST); \
	pthread_mutex_unlock(&state_lock); } while (0)

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
	if (atomic_dec_and_test(&folio->_refcount)) {
		/* The table's reference is dropped only after removal, so a
		 * still-mapped folio reaching zero means a caller over-put. */
		WARN_ON(folio->mapping != NULL);
		folio_free(folio);
	}
}

struct folio *folio_alloc_standalone(gfp_t gfp)
{
	struct folio *f = folio_alloc_new(NULL, 0);

	if (f && (gfp & __GFP_ZERO))
		memset(f->data, 0, PAGE_SIZE);
	return f;
}

static unsigned long g_max_folios = NTFS_PAGECACHE_MAX_FOLIOS;

long pagecache_nr_folios(void)
{
	return (long)atomic64_read(&g_nr_folios);
}

void pagecache_set_max_folios(unsigned long max)
{
	__atomic_store_n(&g_max_folios, max ? max : NTFS_PAGECACHE_MAX_FOLIOS, __ATOMIC_SEQ_CST);
}

static inline long folios_over_cap(void)
{
	return (long)atomic64_read(&g_nr_folios) -
	       (long)__atomic_load_n(&g_max_folios, __ATOMIC_SEQ_CST);
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
	state_changed();
}
void folio_wait_locked(struct folio *folio)
{
	state_wait_while(folio_test_locked(folio));
}

/* ------------------------------------------------------------------ */
/* State bits                                                          */

void folio_mark_uptodate(struct folio *folio) { set_bit(PG_uptodate, &folio->flags); }
void folio_clear_uptodate(struct folio *folio) { clear_bit(PG_uptodate, &folio->flags); }
void folio_set_error(struct folio *folio) { set_bit(PG_error, &folio->flags); }

static void mapping_note_dirty(struct address_space *m);

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
	bool newly, first;

	if (!m)
		return false;		/* truncated away; nothing to write */
	spin_lock(&m->tree_lock);
	newly = __folio_set_dirty(m, folio);
	/* Only the 0 -> 1 transition needs the registry: above one dirty folio the
	 * mapping is certainly still listed, because pruning requires nrdirty == 0.
	 * Without this a thousand-folio write took registry_lock a thousand times
	 * and contended with the sync walk that holds it. */
	first = newly && m->nrdirty == 1;
	spin_unlock(&m->tree_lock);
	if (first)
		mapping_note_dirty(m);	/* after tree_lock: registry_lock is above it */
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
	state_changed();
}
void folio_wait_writeback(struct folio *folio)
{
	state_wait_while(folio_test_writeback(folio));
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
	long over = folios_over_cap();
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
			/* Lost the race to insert; nf was never in the table. */
			mutex_unlock(&nf->lock);
			nf->mapping = NULL;
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
	/*
	 * Kernel semantics (read_cache_folio): an uptodate folio is returned
	 * without taking its lock. The core relies on this - mft.c allocates
	 * an extent record with the $MFT folio locked and then maps that same
	 * folio through map_mft_record(); locking it here would deadlock.
	 * (vfs stream fix, adopted by the platform review.) This is safe
	 * against invalidate_mapping_pages()/reclaim because those decide
	 * "unreferenced" under tree_lock, where this lookup took its
	 * reference. truncate/invalidate2 drop referenced folios by design;
	 * their callers hold the locks that exclude readers.
	 */
	f = __filemap_get_folio(mapping, index, FGP_ACCESSED, 0);
	if (!IS_ERR(f)) {
		if (folio_test_uptodate(f))
			return f;
		folio_put(f);
	}
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

/*
 * Write one folio. Caller holds the folio lock and a reference. Returns a
 * negative errno, 0, or 1 when the backend re-dirtied the folio instead of
 * writing it (the caller may retry).
 *
 * PG_writeback goes up before PG_dirty comes down (see the header comment):
 * a syncing thread that finds the folio clean will find it under writeback
 * and wait, instead of returning while the data is still in flight.
 */
static int writeback_one(struct address_space *m, struct folio *f)
{
	int err;

	folio_start_writeback(f);
	if (!folio_clear_dirty_for_io(f) || f->mapping != m) {
		/* Clean already, or truncated under us. */
		folio_end_writeback(f);
		return 0;
	}
	err = m->a_ops && m->a_ops->write_folio ? m->a_ops->write_folio(m, f) : -EIO;
	if (err) {
		folio_set_error(f);
		spin_lock(&m->tree_lock);
		if (!m->wb_err)
			m->wb_err = err;
		spin_unlock(&m->tree_lock);
	}
	folio_end_writeback(f);
	if (!err && folio_test_dirty(f))
		return 1;		/* redirtied by the backend */
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

/* Write the dirty folios in range once. *redirtied (may be NULL) counts
 * the folios the backend handed back dirty. */
static int write_range(struct address_space *m, pgoff_t first, pgoff_t last,
		       u64 older_than_ns, int *redirtied)
{
	size_t n, i;
	struct folio **arr = collect_dirty(m, first, last, older_than_ns, &n);
	int err = 0, e;

	for (i = 0; i < n; i++) {
		folio_lock(arr[i]);
		e = writeback_one(m, arr[i]);
		folio_unlock(arr[i]);
		folio_put(arr[i]);
		if (e > 0) {
			if (redirtied)
				(*redirtied)++;
		} else if (e && !err) {
			err = e;
		}
	}
	free(arr);
	return err;
}

/* Synchronous flush: retry re-dirtied folios a bounded number of times
 * (the backend's trylock target is normally released within microseconds).
 * If the caller itself holds that lock the retries just cost
 * WB_REDIRTY_RETRIES * WB_REDIRTY_BACKOFF_US and the folio stays dirty for
 * the background writer, exactly as in the kernel. */
#define WB_REDIRTY_RETRIES	16
#define WB_REDIRTY_BACKOFF_US	1000

static int write_range_sync(struct address_space *m, pgoff_t first, pgoff_t last)
{
	int attempt, err;

	for (attempt = 0; ; attempt++) {
		int redirtied = 0;

		err = write_range(m, first, last, 0, &redirtied);
		if (err || !redirtied || attempt >= WB_REDIRTY_RETRIES)
			return err;
		usleep(WB_REDIRTY_BACKOFF_US);
	}
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
	return write_range_sync(mapping, byte_to_index(start),
				end < 0 ? (pgoff_t)-1 : byte_to_index(end));
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
 * the tree_lock is NOT held. Drops the table's reference. With @only_unused
 * the removal happens only if nobody but the table and the caller holds a
 * reference and the folio is clean and not under write-back; that check is
 * made under tree_lock, where lookups take their references, so it is
 * atomic with them (the kernel's folio_ref_freeze() under the xarray lock).
 * Returns true if the folio was removed.
 */
static bool __remove_locked_cond(struct address_space *m, struct folio *f, bool only_unused)
{
	spin_lock(&m->tree_lock);
	if (f->mapping != m ||
	    (only_unused && (atomic_read(&f->_refcount) != 2 || folio_test_dirty(f) ||
			     folio_test_writeback(f)))) {
		spin_unlock(&m->tree_lock);
		return false;
	}
	__folio_clear_dirty(m, f);
	idx_remove(idx_of(m), f);
	list_del_init(&f->lru);
	m->nrpages--;
	f->mapping = NULL;
	spin_unlock(&m->tree_lock);
	folio_put(f);
	return true;
}

static void __remove_locked(struct address_space *m, struct folio *f)
{
	__remove_locked_cond(m, f, false);
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
			/* Our ref + table ref == 2 means nobody else; decided
			 * under tree_lock so a concurrent lookup cannot hand
			 * out a folio we are dropping. */
			if (__remove_locked_cond(m, f, true))
				dropped++;
		} else {
			folio_lock(f);
			folio_wait_writeback(f);
			if (mode == 2 && atomic_read(&f->_refcount) != 2 && !err)
				err = -EBUSY;
			if (f->mapping == m) {
				__remove_locked(m, f);
				dropped++;
			}
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
static void mapping_busy_dec(struct pc_index *ix)	/* registry_lock held */
{
	/* busy is read by address_space_destroy() under state_lock. */
	pthread_mutex_lock(&state_lock);
	ix->busy--;
	pthread_mutex_unlock(&state_lock);
	state_changed();
}

/*
 * Run @fn(mapping) for every registered mapping (optionally only those of
 * @sb) without holding registry_lock across the call, in registration
 * order or, with @reverse, newest first.
 */
static void mapping_note_dirty(struct address_space *m)
{
	struct pc_index *ix = idx_of(m);

	if (!ix)
		return;
	pthread_mutex_lock(&registry_lock);
	if (list_empty(&ix->dirty_registry))
		list_add_tail(&ix->dirty_registry, &g_dirty);
	pthread_mutex_unlock(&registry_lock);
}

/*
 * Like for_each_mapping(), but over the maybe-dirty list, and it prunes.
 *
 * @prune drops a mapping from the list once @fn has returned and the mapping
 * is clean. That check runs under registry_lock, which the adder also takes, so
 * a folio dirtied concurrently either raises nrdirty before we look (we keep
 * the entry) or adds the entry back after we drop it. It runs under tree_lock
 * as well, which is legal here and nowhere near a lock-order inversion:
 * registry_lock is the outer lock (see the header comment).
 *
 * Pruning is only correct on a pass that waited for the mapping's writeback --
 * nrdirty reaches 0 the moment writeback starts, not when it finishes -- so the
 * caller passes prune only for a pass that ran filemap_write_and_wait().
 */
static int for_each_dirty_mapping(struct super_block *sb, bool reverse, bool prune,
				  int (*fn)(struct address_space *, void *), void *arg)
{
	struct pc_index cursor = { .mapping = NULL };
	struct list_head *pos;
	int err = 0;

	pthread_mutex_lock(&registry_lock);
	for (pos = reverse ? g_dirty.prev : g_dirty.next; pos != &g_dirty;
	     pos = reverse ? pos->prev : pos->next) {
		struct pc_index *ix = list_entry(pos, struct pc_index, dirty_registry);
		struct address_space *m = ix->mapping;
		bool drop = false;
		int e;

		if (!m)
			continue;	/* another iterator's cursor */
		if (sb && (!m->host || m->host->i_sb != sb))
			continue;
		if (reverse)
			list_add_tail(&cursor.dirty_registry, pos);
		else
			list_add(&cursor.dirty_registry, pos);
		mapping_busy_inc(ix);
		pthread_mutex_unlock(&registry_lock);
		e = fn(m, arg);
		if (e && !err)
			err = e;
		pthread_mutex_lock(&registry_lock);
		mapping_busy_dec(ix);
		if (prune && !e) {
			spin_lock(&m->tree_lock);
			drop = m->nrdirty == 0;
			spin_unlock(&m->tree_lock);
		}
		/* Unlink ix BEFORE reading the resume position out of the cursor.
		 * The cursor sits next to ix, so the resume node is ix itself;
		 * dropping ix afterwards left pos on a node whose next pointed at
		 * itself and the walk span forever. */
		if (drop)
			list_del_init(&ix->dirty_registry);
		pos = reverse ? cursor.dirty_registry.next : cursor.dirty_registry.prev;
		list_del(&cursor.dirty_registry);
	}
	pthread_mutex_unlock(&registry_lock);
	return err;
}

static int for_each_mapping(struct super_block *sb, bool reverse,
			    int (*fn)(struct address_space *, void *), void *arg)
{
	struct pc_index cursor = { .mapping = NULL };	/* skipped by others */
	struct list_head *pos;
	int err = 0;

	pthread_mutex_lock(&registry_lock);
	for (pos = reverse ? g_registry.prev : g_registry.next; pos != &g_registry;
	     pos = reverse ? pos->prev : pos->next) {
		struct pc_index *ix = list_entry(pos, struct pc_index, registry);
		struct address_space *m = ix->mapping;
		int e;
		if (!m)
			continue;	/* another iterator's cursor */
		if (sb && (!m->host || m->host->i_sb != sb))
			continue;
		/* Park a cursor next to ix (on the side we came from) so a
		 * concurrent destroy of ix cannot strand us; busy>0 keeps
		 * ix's memory alive meanwhile. */
		if (reverse)
			list_add_tail(&cursor.registry, pos);	/* before ix */
		else
			list_add(&cursor.registry, pos);	/* after ix */
		mapping_busy_inc(ix);
		pthread_mutex_unlock(&registry_lock);
		e = fn(m, arg);
		if (e && !err)
			err = e;
		pthread_mutex_lock(&registry_lock);
		mapping_busy_dec(ix);
		/* Resume from the cursor: the loop step moves past it. */
		pos = reverse ? cursor.registry.next : cursor.registry.prev;
		list_del(&cursor.registry);
	}
	pthread_mutex_unlock(&registry_lock);
	return err;
}

static int writer_pass_fn(struct address_space *m, void *arg)
{
	u64 cutoff = *(u64 *)arg;
	int err = write_range(m, 0, (pgoff_t)-1, cutoff, NULL);
	long over;

	/* Global reclaim when over the cap. */
	over = folios_over_cap();
	if (over > 0) {
		spin_lock(&m->tree_lock);
		__reclaim_from(m, over);
		spin_unlock(&m->tree_lock);
	}
	return err;
}

/*
 * pagecache_sync_sb ordering. NTFS has no ordering guarantee from the
 * kernel either, but for crash consistency the MFT records that reference
 * clusters, index blocks and MFT-record slots should hit the device after
 * the structures they point at: a bitmap written without its MFT record
 * leaks space (chkdsk reclaims it), an MFT record written without its
 * bitmap bit lets the next allocation reuse live clusters/records. So the
 * mappings of inode 0 ($MFT/$DATA and its attribute inodes, e.g.
 * $MFT/$BITMAP, which the driver gives i_ino 0 as well) go in a second pass,
 * newest first, which puts $MFT/$BITMAP (created after $MFT/$DATA) before
 * the MFT data itself. Note that mft.c writes MFT records synchronously
 * through bios in write_inode; this ordering covers the folios flushed by
 * the cache (bitmaps, index allocations, resident-attribute views, and MFT
 * folios dirtied in place).
 */
static bool mapping_syncs_last(const struct address_space *m)
{
	return m->host && m->host->i_ino == 0;
}

static int sync_fn(struct address_space *m, void *arg)
{
	bool last_pass = *(bool *)arg;

	if (mapping_syncs_last(m) != last_pass)
		return 0;
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
		for_each_mapping(NULL, false, writer_pass_fn, &cutoff);
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
	bool first = false, last = true;
	int err, e;

	err = for_each_dirty_mapping(sb, false, false, sync_fn, &first);
	e = for_each_dirty_mapping(sb, true, true, sync_fn, &last);
	return err ? err : e;
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
	INIT_LIST_HEAD(&ix->dirty_registry);
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
	list_del_init(&ix->dirty_registry);
	pthread_mutex_unlock(&registry_lock);
	state_wait_while(ix->busy);

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
