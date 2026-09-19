/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * The inode table (linux/fs.h) for the user-space port: a generic, kernel
 * shaped icache. The file-system specific parts come through
 * super_operations (alloc/free/drop/evict/write_inode).
 *
 * Locking (outer to inner): sb->s_inode_list_lock > inode->i_lock >
 * lru_lock.
 *  - The sb lock protects the per-sb hash and the sb inode list; lookups
 *    take their reference under it.
 *  - i_lock protects i_state and, as in the kernel, makes the last-reference
 *    decision of iput() atomic with igrab(): iput() drops the count and, if
 *    it decides to evict, marks the inode I_WILL_FREE before releasing
 *    i_lock; igrab() checks that mark under the same lock. Eviction from the
 *    LRU (cap, unmount) claims its victim the same way, so an igrab() that
 *    wins the race simply revives the inode.
 *  - lru_lock (global, innermost) protects the per-sb LRU lists so igrab(),
 *    which may run without the sb lock, can leave the LRU safely.
 *  - i_new_lock is held by the creator of an I_NEW inode until
 *    unlock_new_inode(); waiters lock/unlock it to block. Inodes being torn
 *    down carry I_WILL_FREE/I_FREEING and are skipped by lookups, which wait
 *    for them to leave the hash.
 *
 * Caching: the last iput() of a cacheable inode (nlink > 0, still hashed,
 * drop_inode says no) parks it on the sb's LRU instead of evicting it. Once
 * the LRU exceeds NTFS_INODE_CACHE_MAX the oldest entries are written back
 * and evicted. evict_inodes() (unmount) drains everything unreferenced.
 */
#include <stdlib.h>
#include <stdatomic.h>
#include <linux/blkdev.h>
#include <string.h>
#include <pthread.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/dcache.h>
#include <ntfsport/bdev.h>

/* Bumped (with a broadcast) whenever an inode leaves a hash; lookups that
 * find a dying inode wait for the next bump and rescan. */
static pthread_mutex_t freed_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t freed_cond = PTHREAD_COND_INITIALIZER;
static u64 freed_gen;

#define sb_lock(sb)	spin_lock(&(sb)->s_inode_list_lock)
#define sb_unlock(sb)	spin_unlock(&(sb)->s_inode_list_lock)

/* ------------------------------------------------------------------ */
/* Hash                                                                */

static inline struct hlist_head *inode_hash_head(struct super_block *sb, unsigned long hashval)
{
	u64 h = (u64)hashval * 0x9E3779B97F4A7C15ULL;
	return &sb->s_inode_hash[h >> (64 - sb->s_inode_hash_bits)];
}

static void freed_gen_bump(void)
{
	pthread_mutex_lock(&freed_lock);
	freed_gen++;
	pthread_cond_broadcast(&freed_cond);
	pthread_mutex_unlock(&freed_lock);
}

/* Called with sb locked; drops and retakes it. */
static void wait_on_freeing(struct super_block *sb)
{
	u64 gen;
	struct timespec ts;

	pthread_mutex_lock(&freed_lock);
	gen = freed_gen;
	pthread_mutex_unlock(&freed_lock);
	sb_unlock(sb);
	pthread_mutex_lock(&freed_lock);
	while (freed_gen == gen) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&freed_cond, &freed_lock, &ts);
	}
	pthread_mutex_unlock(&freed_lock);
	sb_lock(sb);
}

static inline bool inode_dying(const struct inode *inode)
{
	return inode_state_read_once(inode) & (I_FREEING | I_WILL_FREE);
}

/* ------------------------------------------------------------------ */
/* LRU of unreferenced inodes (lru_lock, innermost)                    */

unsigned long inode_cache_global_count;
static pthread_mutex_t lru_lock = PTHREAD_MUTEX_INITIALIZER;

static void __lru_remove_locked(struct inode *inode)
{
	if (!list_empty(&inode->i_lru)) {
		list_del_init(&inode->i_lru);
		inode->i_sb->s_nr_inode_lru--;
		__atomic_fetch_sub(&inode_cache_global_count, 1, __ATOMIC_RELAXED);
	}
}

static void lru_add(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	pthread_mutex_lock(&lru_lock);
	if (list_empty(&inode->i_lru)) {
		list_add_tail(&inode->i_lru, &sb->s_inode_lru);
		sb->s_nr_inode_lru++;
		__atomic_fetch_add(&inode_cache_global_count, 1, __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&lru_lock);
}

static void lru_remove(struct inode *inode)
{
	pthread_mutex_lock(&lru_lock);
	__lru_remove_locked(inode);
	pthread_mutex_unlock(&lru_lock);
}

/* Pop the oldest cached inode if the cap is exceeded; sb locked. */
static struct inode *lru_pop_over_cap(struct super_block *sb)
{
	struct inode *victim = NULL;

	pthread_mutex_lock(&lru_lock);
	if (sb->s_nr_inode_lru > NTFS_INODE_CACHE_MAX) {
		victim = list_first_entry(&sb->s_inode_lru, struct inode, i_lru);
		__lru_remove_locked(victim);
	}
	pthread_mutex_unlock(&lru_lock);
	return victim;
}

/* Take a reference; sb locked. A 0->1 transition leaves the LRU. */
static void __iget(struct inode *inode)
{
	if (atomic_inc_return(&inode->i_count) == 1)
		lru_remove(inode);
}

/*
 * Claim an unreferenced inode for eviction: with i_lock held, verify that
 * nobody revived it (igrab) meanwhile and mark it I_WILL_FREE so nobody
 * can from now on. Returns false if it is referenced or already dying.
 */
static bool claim_for_eviction(struct inode *inode)
{
	bool ok;

	spin_lock(&inode->i_lock);
	ok = atomic_read(&inode->i_count) == 0 && !inode_dying(inode);
	if (ok)
		inode->i_state |= I_WILL_FREE;
	spin_unlock(&inode->i_lock);
	return ok;
}

unsigned long inode_cache_count(void)
{
	/* Tests use one sb at a time; report the global count through a
	 * walk-free counter kept per sb would need the sb, so keep a global
	 * mirror. */
	extern unsigned long inode_cache_global_count;
	return __atomic_load_n(&inode_cache_global_count, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* Allocation / initialisation                                         */

/* Monotonic, so "newer" survives a walk over any list order. */
static unsigned long next_inode_seq(void)
{
	static _Atomic unsigned long seq;

	return atomic_fetch_add(&seq, 1) + 1;
}

void inode_init_once(struct inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_sb_list);
	INIT_LIST_HEAD(&inode->i_dirty_list);
	INIT_LIST_HEAD(&inode->i_lru);
	inode->i_seq = next_inode_seq();
	spin_lock_init(&inode->i_lock);
	init_rwsem(&inode->i_rwsem);
	mutex_init(&inode->i_new_lock);
	atomic_set(&inode->i_count, 0);
}

static int inode_init_always(struct super_block *sb, struct inode *inode)
{
	inode->i_sb = sb;
	inode->i_blkbits = sb->s_blocksize_bits;
	inode->i_flags = 0;
	inode->i_state = 0;
	atomic_set(&inode->i_count, 1);
	inode->i_op = NULL;
	inode->i_fop = NULL;
	inode->i_ino = 0;
	inode->i_opflags = 0;
	inode->i_nlink = 1;
	inode->i_uid = KUIDT_INIT(0);
	inode->i_gid = KGIDT_INIT(0);
	inode->i_size = 0;
	inode->i_blocks = 0;
	inode->i_bytes = 0;
	inode->i_generation = 0;
	inode->i_rdev = 0;
	inode->i_version = 0;
	inode->i_private = NULL;
	memset(&inode->i_atime, 0, sizeof(inode->i_atime));
	memset(&inode->i_mtime, 0, sizeof(inode->i_mtime));
	memset(&inode->i_ctime, 0, sizeof(inode->i_ctime));
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_sb_list);
	INIT_LIST_HEAD(&inode->i_dirty_list);
	INIT_LIST_HEAD(&inode->i_lru);
	inode->i_seq = next_inode_seq();
	address_space_init(&inode->i_data, inode, NULL);
	inode->i_mapping = &inode->i_data;
	return 0;
}

static struct inode *alloc_inode(struct super_block *sb)
{
	const struct super_operations *ops = sb->s_op;
	struct inode *inode;

	if (ops && ops->alloc_inode) {
		inode = ops->alloc_inode(sb);
	} else {
		inode = malloc(sizeof(*inode));
		if (inode)
			inode_init_once(inode);
	}
	if (!inode)
		return NULL;
	inode_init_always(sb, inode);
	return inode;
}

static void free_inode_mem(struct inode *inode)
{
	const struct super_operations *ops = inode->i_sb->s_op;

	if (ops && ops->destroy_inode)
		ops->destroy_inode(inode);
	if (ops && ops->free_inode)
		ops->free_inode(inode);
	else if (!(ops && ops->destroy_inode))
		free(inode);
}

/* Undo alloc_inode() for an inode that never became visible. */
static void discard_new_inode(struct inode *inode)
{
	address_space_destroy(&inode->i_data, true);
	free_inode_mem(inode);
}

struct inode *new_inode(struct super_block *sb)
{
	struct inode *inode = alloc_inode(sb);

	if (!inode)
		return NULL;
	sb_lock(sb);
	list_add(&inode->i_sb_list, &sb->s_inodes);
	sb_unlock(sb);
	return inode;
}

void inode_init_owner(struct mnt_idmap *idmap, struct inode *inode,
		      const struct inode *dir, umode_t mode)
{
	(void)idmap;
	inode->i_uid = current_fsuid();
	if (dir && (dir->i_mode & S_ISGID)) {
		inode->i_gid = dir->i_gid;
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	} else {
		inode->i_gid = current_fsgid();
	}
	inode->i_mode = mode;
}

/* ------------------------------------------------------------------ */
/* Hash maintenance                                                    */

static void __remove_hash_locked(struct inode *inode)
{
	if (!hlist_unhashed(&inode->i_hash)) {
		hlist_del_init(&inode->i_hash);
		freed_gen_bump();
	}
}

void insert_inode_hash(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	sb_lock(sb);
	hlist_add_head(&inode->i_hash, inode_hash_head(sb, inode->i_ino));
	sb_unlock(sb);
}

void remove_inode_hash(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	sb_lock(sb);
	__remove_hash_locked(inode);
	sb_unlock(sb);
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */

/*
 * Find a live inode matching @test. sb locked on entry and exit; may drop
 * it to wait for a dying inode. Returns with a reference taken.
 */
static struct inode *find_inode(struct super_block *sb, unsigned long hashval,
				int (*test)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hash_head(sb, hashval);
	struct inode *inode;

repeat:
	hlist_for_each_entry(inode, head, i_hash) {
		if (inode->i_sb != sb)
			continue;
		if (!test(inode, data))
			continue;
		if (inode_dying(inode)) {
			wait_on_freeing(sb);
			goto repeat;
		}
		__iget(inode);
		return inode;
	}
	return NULL;
}

/* Block until @inode leaves I_NEW. Caller holds a reference. */
static void wait_on_inode(struct inode *inode)
{
	while (inode_state_read_once(inode) & I_NEW) {
		mutex_lock(&inode->i_new_lock);
		mutex_unlock(&inode->i_new_lock);
	}
}

struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
			   int (*test)(struct inode *, void *),
			   int (*set)(struct inode *, void *), void *data)
{
	struct inode *inode, *new;

again:
	sb_lock(sb);
	inode = find_inode(sb, hashval, test, data);
	sb_unlock(sb);
	if (inode) {
found:
		wait_on_inode(inode);
		if (unlikely(inode_unhashed(inode))) {
			iput(inode);
			goto again;
		}
		return inode;
	}

	/* Allocate under the sb lock: user-space allocation never sleeps on
	 * I/O, and this guarantees a racing lookup creates exactly one inode. */
	sb_lock(sb);
	inode = find_inode(sb, hashval, test, data);
	if (inode) {
		sb_unlock(sb);
		goto found;
	}
	new = alloc_inode(sb);
	if (!new) {
		sb_unlock(sb);
		return NULL;
	}
	if (set(new, data)) {
		sb_unlock(sb);
		discard_new_inode(new);
		return NULL;
	}
	new->i_state = I_NEW;
	mutex_lock(&new->i_new_lock);
	hlist_add_head(&new->i_hash, inode_hash_head(sb, hashval));
	list_add(&new->i_sb_list, &sb->s_inodes);
	sb_unlock(sb);
	return new;
}

struct inode *ilookup5_nowait(struct super_block *sb, unsigned long hashval,
			      int (*test)(struct inode *, void *), void *data)
{
	struct inode *inode;

	sb_lock(sb);
	inode = find_inode(sb, hashval, test, data);
	sb_unlock(sb);
	return inode;
}

struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
		       int (*test)(struct inode *, void *), void *data)
{
	struct inode *inode;

again:
	inode = ilookup5_nowait(sb, hashval, test, data);
	if (inode) {
		wait_on_inode(inode);
		if (unlikely(inode_unhashed(inode))) {
			iput(inode);
			goto again;
		}
	}
	return inode;
}

/*
 * The match callback runs with the sb lock held and is responsible for
 * skipping inodes it does not want and for taking the reference (igrab)
 * when it returns 1; -1 stops the search.
 */
struct inode *(find_inode_nowait)(struct super_block *sb, unsigned long hashval,
				int (*match)(struct inode *, unsigned long, void *), void *data)
{
	struct hlist_head *head = inode_hash_head(sb, hashval);
	struct inode *inode, *ret = NULL;

	sb_lock(sb);
	hlist_for_each_entry(inode, head, i_hash) {
		int mval;

		if (inode->i_sb != sb)
			continue;
		mval = match(inode, hashval, data);
		if (mval == 0)
			continue;
		if (mval == 1)
			ret = inode;
		break;
	}
	sb_unlock(sb);
	return ret;
}

#undef find_inode_nowait
struct inode *find_inode_nowait_u64(struct super_block *sb, u64 hashval,
				    int (*match)(struct inode *, u64, void *), void *data)
{
	struct hlist_head *head = inode_hash_head(sb, (unsigned long)hashval);
	struct inode *inode, *ret = NULL;

	sb_lock(sb);
	hlist_for_each_entry(inode, head, i_hash) {
		int mval;

		if (inode->i_sb != sb)
			continue;
		mval = match(inode, hashval, data);
		if (mval == 0)
			continue;
		if (mval == 1)
			ret = inode;
		break;
	}
	sb_unlock(sb);
	return ret;
}

/* ------------------------------------------------------------------ */
/* References                                                          */

void ihold(struct inode *inode)
{
	WARN_ON(atomic_inc_return(&inode->i_count) < 2);
}

struct inode *igrab(struct inode *inode)
{
	struct inode *ret = NULL;

	spin_lock(&inode->i_lock);
	if (!inode_dying(inode)) {
		/* Atomic with iput()'s last-reference decision (i_lock). A
		 * 0->1 transition revives a cached inode: leave the LRU
		 * (lru_lock nests inside i_lock). */
		if (atomic_inc_return(&inode->i_count) == 1)
			lru_remove(inode);
		ret = inode;
	}
	spin_unlock(&inode->i_lock);
	return ret;
}

void unlock_new_inode(struct inode *inode)
{
	spin_lock(&inode->i_lock);
	WARN_ON(!(inode->i_state & I_NEW));
	inode->i_state &= ~(I_NEW | I_CREATING);
	spin_unlock(&inode->i_lock);
	mutex_unlock(&inode->i_new_lock);
}

void iget_failed(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	/* Leave the hash first so waiters retry instead of adopting us. */
	sb_lock(sb);
	__remove_hash_locked(inode);
	sb_unlock(sb);
	clear_nlink(inode);
	unlock_new_inode(inode);
	iput(inode);
}

void clear_inode(struct inode *inode)
{
	spin_lock(&inode->i_lock);
	inode->i_state = I_FREEING | I_CLEAR;
	spin_unlock(&inode->i_lock);
}

int generic_delete_inode(struct inode *inode)
{
	(void)inode;
	return 1;
}

int generic_drop_inode(struct inode *inode)
{
	return !inode->i_nlink || inode_unhashed(inode);
}

/* I_FREEING set, no references, not on the LRU; sb not locked. */
static void evict(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	const struct super_operations *op = sb->s_op;

	if (op && op->evict_inode) {
		op->evict_inode(inode);
	} else {
		truncate_inode_pages_final(&inode->i_data);
		clear_inode(inode);
	}
	sb_lock(sb);
	__remove_hash_locked(inode);
	list_del_init(&inode->i_sb_list);
	list_del_init(&inode->i_dirty_list);
	sb_unlock(sb);
	address_space_destroy(&inode->i_data, true);
	free_inode_mem(inode);
}

/* Write back (if @write) and evict an inode that has been claimed
 * (I_WILL_FREE set, i_count == 0, off the LRU). Called with the sb lock
 * held; returns with it dropped. */
static void evict_unreferenced_locked(struct inode *inode, bool write)
{
	struct super_block *sb = inode->i_sb;
	unsigned long state;

	if (write) {
		sb_unlock(sb);
		write_inode_now(inode, 1);
		sb_lock(sb);
	}
	spin_lock(&inode->i_lock);
	state = inode->i_state & ~I_WILL_FREE;
	inode->i_state = state | I_FREEING;
	spin_unlock(&inode->i_lock);
	sb_unlock(sb);
	evict(inode);
}

static void shrink_lru(struct super_block *sb)
{
	for (;;) {
		struct inode *victim;

		sb_lock(sb);
		victim = lru_pop_over_cap(sb);
		if (!victim) {
			sb_unlock(sb);
			return;
		}
		if (!claim_for_eviction(victim)) {
			/* Revived by igrab() after we popped it; it goes back
			 * on the LRU with its next iput(). */
			sb_unlock(sb);
			continue;
		}
		evict_unreferenced_locked(victim, victim->i_nlink != 0);
	}
}

void iput(struct inode *inode)
{
	struct super_block *sb;
	const struct super_operations *op;
	int drop;

	if (!inode)
		return;
	sb = inode->i_sb;
	op = sb->s_op;
	sb_lock(sb);
	spin_lock(&inode->i_lock);
	if (atomic_dec_return(&inode->i_count) > 0) {
		spin_unlock(&inode->i_lock);
		sb_unlock(sb);
		return;
	}
	WARN_ON(inode_state_read_once(inode) & I_NEW);
	drop = op && op->drop_inode ? op->drop_inode(inode) : generic_drop_inode(inode);
	if (!drop && (sb->s_flags & SB_ACTIVE)) {
		lru_add(inode);
		spin_unlock(&inode->i_lock);
		sb_unlock(sb);
		shrink_lru(sb);
		return;
	}
	/* Evicting: from here on igrab() fails (it checks under i_lock). */
	inode->i_state |= I_WILL_FREE;
	spin_unlock(&inode->i_lock);
	evict_unreferenced_locked(inode, !drop);
}

/* ------------------------------------------------------------------ */
/* Dirty state and write-back                                          */

/*
 * Put @inode on its sb's maybe-dirty list. Same one-sided invariant as the page
 * cache's g_dirty (see pagecache.c): everything that needs writing back is
 * listed, something listed may already be clean. sync_inodes_sb() used to walk
 * every inode on the volume on every sync, and FSKit syncs about every second
 * operation, which made each unlink O(cached inodes).
 *
 * Called with no lock held: i_lock is dropped first, so this never nests.
 */
void inode_note_dirty(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	if (!sb)
		return;
	sb_lock(sb);
	if (list_empty(&inode->i_dirty_list) && !list_empty(&inode->i_sb_list))
		list_add_tail(&inode->i_dirty_list, &sb->s_dirty_inodes);
	sb_unlock(sb);
}

void __mark_inode_dirty(struct inode *inode, int flags)
{
	bool was, now;

	spin_lock(&inode->i_lock);
	was = (inode->i_state & I_DIRTY_ALL) != 0;
	inode->i_state |= flags & I_DIRTY_ALL;
	now = (inode->i_state & I_DIRTY_ALL) != 0;
	spin_unlock(&inode->i_lock);
	/* Only the clean -> dirty edge: while it stays dirty it stays listed,
	 * because pruning requires it to be clean. */
	if (!was && now)
		inode_note_dirty(inode);
}

int write_inode_now(struct inode *inode, int sync)
{
	const struct super_operations *op = inode->i_sb->s_op;
	struct writeback_control wbc = {
		.nr_to_write = LONG_MAX,
		.range_start = 0,
		.range_end = -1,
		.sync_mode = sync ? WB_SYNC_ALL : WB_SYNC_NONE,
	};
	unsigned long dirty;
	int err = 0, err2;

	if (inode->i_mapping) {
		if (sync)
			err = filemap_write_and_wait(inode->i_mapping);
		else
			err = filemap_fdatawrite_range(inode->i_mapping, 0, -1);
	}
	spin_lock(&inode->i_lock);
	dirty = inode->i_state & I_DIRTY_ALL;
	inode->i_state = (inode->i_state & ~I_DIRTY_ALL) | I_SYNC;
	spin_unlock(&inode->i_lock);
	if (dirty && op && op->write_inode) {
		err2 = op->write_inode(inode, &wbc);
		if (err2) {
			spin_lock(&inode->i_lock);
			inode->i_state |= dirty;
			spin_unlock(&inode->i_lock);
			if (!err)
				err = err2;
		}
	}
	spin_lock(&inode->i_lock);
	inode->i_state &= ~I_SYNC;
	spin_unlock(&inode->i_lock);
	return err;
}

/*
 * Write-back order for sync_inodes_sb() (see docs/progress/platform-review.md):
 * system inodes 1..15 ($MFTMirr, $LogFile, $Volume, root, $Bitmap, ...)
 * first, then user inodes, then inode 0 ($MFT/$DATA and its attribute inodes
 * such as $MFT/$BITMAP) last, newest first, so the bitmaps reach the device
 * before the MFT records whose allocations they cover. Within one inode,
 * write_inode_now() flushes its folios (index blocks, data) before its MFT
 * record.
 */
static int sync_rank(const struct inode *inode)
{
	if (inode->i_ino == 0)
		return 2;
	return inode->i_ino < 16 ? 0 : 1;
}

static int cmp_sync_order(const void *a, const void *b)
{
	const struct inode *x = *(struct inode * const *)a;
	const struct inode *y = *(struct inode * const *)b;
	int rx = sync_rank(x), ry = sync_rank(y);

	if (rx != ry)
		return rx < ry ? -1 : 1;
	if (x->i_seq != y->i_seq)
		return x->i_seq > y->i_seq ? -1 : 1;	/* newest first */
	return 0;
}

/* Snapshot the sb's inodes (referenced) so callbacks run unlocked; the array is
 * in sync_rank order, and newest first within a rank. */
static struct inode **snapshot_inodes(struct super_block *sb, size_t *count, bool dirty_only)
{
	struct inode **arr = NULL;
	struct inode *inode;
	struct list_head *head, *pos;
	size_t n = 0, cap = 0;

	sb_lock(sb);
	/* Explicit traversal: the list member differs between the two lists and
	 * list_for_each_entry() takes it as a token, not a value. */
	head = dirty_only ? &sb->s_dirty_inodes : &sb->s_inodes;
	for (pos = head->next; pos != head; pos = pos->next) {
		inode = dirty_only ? list_entry(pos, struct inode, i_dirty_list)
				   : list_entry(pos, struct inode, i_sb_list);
		if (inode_dying(inode) || (inode_state_read_once(inode) & I_NEW))
			continue;
		if (dirty_only && !(inode_state_read_once(inode) & I_DIRTY_ALL) &&
		    !(inode->i_mapping && inode->i_mapping->nrdirty))
			continue;
		if (n == cap) {
			size_t ncap = cap ? cap * 2 : 64;
			struct inode **na = realloc(arr, ncap * sizeof(*arr));
			if (!na)
				break;
			arr = na;
			cap = ncap;
		}
		__iget(inode);
		arr[n++] = inode;
	}
	sb_unlock(sb);
	/* Rank ascending, then newest first within a rank. i_seq is unique, so
	 * this is a total order and qsort's instability does not matter. */
	qsort(arr, n, sizeof(*arr), cmp_sync_order);
	*count = n;
	return arr;
}

int sync_inodes_sb(struct super_block *sb)
{
	struct inode **arr;
	size_t i, n;
	int err = 0;

	arr = snapshot_inodes(sb, &n, true);
	for (i = 0; i < n; i++) {
		int e = write_inode_now(arr[i], 1);
		if (e && !err)
			err = e;
		/*
		 * Drop it from the maybe-dirty list only here. write_inode_now()
		 * with sync=1 has just flushed the mapping and waited for its
		 * writeback, so this is the one point where "clean" also means
		 * "nothing in flight". sb_lock serialises against a concurrent
		 * dirtier, which either sets the state before this check or adds
		 * the inode back afterwards.
		 */
		if (!e) {
			struct inode *in = arr[i];

			sb_lock(sb);
			if (!(inode_state_read_once(in) & I_DIRTY_ALL) &&
			    !(in->i_mapping && in->i_mapping->nrdirty))
				list_del_init(&in->i_dirty_list);
			sb_unlock(sb);
		}
		iput(arr[i]);
	}
	free(arr);
	return err;
}

int sync_filesystem(struct super_block *sb)
{
	int err, e;

	err = sync_inodes_sb(sb);
	e = pagecache_sync_sb(sb);
	if (e && !err)
		err = e;
	if (sb->s_op && sb->s_op->sync_fs) {
		e = sb->s_op->sync_fs(sb, 1);
		if (e && !err)
			err = e;
	}
	/*
	 * Linux ends sync_filesystem() with sync_blockdev(), which is page-cache
	 * writeback, not a device flush. The barrier is the filesystem's job and
	 * ntfs_sync_fs() issues it via blkdev_issue_flush() just above. This used
	 * to be a third ntfs_bdev_flush() on top of the two that chain already
	 * performs; see the note in linux/blkdev.h.
	 */
	if (sb->s_bdev) {
		e = sync_blockdev(sb->s_bdev);
		if (e && !err)
			err = e;
	}
	return err;
}

void evict_inodes(struct super_block *sb)
{
	struct inode *inode;

	sb->s_flags &= ~SB_ACTIVE;
	for (;;) {
		struct inode *victim = NULL;

		sb_lock(sb);
		list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
			if (atomic_read(&inode->i_count) || inode_dying(inode))
				continue;
			if (!claim_for_eviction(inode))
				continue;
			victim = inode;
			break;
		}
		if (!victim) {
			sb_unlock(sb);
			break;
		}
		lru_remove(victim);
		evict_unreferenced_locked(victim, victim->i_nlink != 0);
	}
	sb_lock(sb);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list)
		if (!inode_dying(inode))
			platform_log(PLATFORM_LOG_WARN, "inode %lu still referenced (%d) after evict_inodes",
				     inode->i_ino, atomic_read(&inode->i_count));
	sb_unlock(sb);
}

/* ------------------------------------------------------------------ */
/* Super block                                                         */

struct super_block *sb_alloc(void)
{
	struct super_block *sb = calloc(1, sizeof(*sb));

	if (!sb)
		return NULL;
	sb->s_inode_hash_bits = 12;
	sb->s_inode_hash = calloc(1UL << sb->s_inode_hash_bits, sizeof(struct hlist_head));
	if (!sb->s_inode_hash) {
		free(sb);
		return NULL;
	}
	INIT_LIST_HEAD(&sb->s_inodes);
	INIT_LIST_HEAD(&sb->s_dirty_inodes);
	INIT_LIST_HEAD(&sb->s_inode_lru);
	spin_lock_init(&sb->s_inode_list_lock);
	init_rwsem(&sb->s_umount);
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1000000000;
	/* Caching of unreferenced inodes is on from the start; evict_inodes()
	 * clears it for unmount. */
	sb->s_flags = SB_ACTIVE;
	return sb;
}

void sb_free(struct super_block *sb)
{
	if (!sb)
		return;
	if (!list_empty(&sb->s_inodes))
		evict_inodes(sb);
	if (!list_empty(&sb->s_inodes))
		platform_log(PLATFORM_LOG_WARN, "sb_free: busy inodes remain");
	free(sb->s_root);
	free(sb->s_inode_hash);
	free(sb);
}

int sb_set_blocksize(struct super_block *sb, int size)
{
	if (size < 512 || size > (int)PAGE_SIZE || !is_power_of_2((unsigned long)size))
		return 0;
	if (sb->s_bdev && (u32)size < bdev_logical_block_size(sb->s_bdev))
		return 0;
	sb->s_blocksize = (unsigned long)size;
	sb->s_blocksize_bits = (unsigned char)ilog2((unsigned long)size);
	return size;
}

int sb_min_blocksize(struct super_block *sb, int size)
{
	int minsize = sb->s_bdev ? (int)bdev_logical_block_size(sb->s_bdev) : 512;

	if (size < minsize)
		size = minsize;
	return sb_set_blocksize(sb, size);
}

struct dentry *d_make_root(struct inode *root)
{
	struct dentry *d;

	if (!root)
		return NULL;
	d = calloc(1, sizeof(*d));
	if (!d) {
		iput(root);
		return NULL;
	}
	d->d_inode = root;
	d->d_sb = root->i_sb;
	d->d_name.name = (const unsigned char *)"/";
	d->d_name.len = 1;
	d->d_parent = d;
	return d;
}
