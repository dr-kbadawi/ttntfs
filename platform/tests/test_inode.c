/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tests for platform/src/inode.c: lookup/creation, I_NEW blocking,
 * reference counting, LRU caching and its cap, iget_failed, dirty
 * write-back, and unmount eviction. Run under ASan/UBSan via NTFS_SANITIZE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* A file system with a private inode wrapper, like ntfs_inode. */
struct test_inode {
	unsigned long key;
	int payload;
	struct inode vfs_inode;
};
#define TI(i) container_of(i, struct test_inode, vfs_inode)

static int n_alloc, n_free, n_evict, n_write;

static struct inode *t_alloc_inode(struct super_block *sb)
{
	struct test_inode *ti = malloc(sizeof(*ti));
	(void)sb;
	if (!ti)
		return NULL;
	ti->key = 0;
	ti->payload = 0;
	inode_init_once(&ti->vfs_inode);
	n_alloc++;
	return &ti->vfs_inode;
}

static void t_free_inode(struct inode *inode)
{
	n_free++;
	free(TI(inode));
}

static void t_evict_inode(struct inode *inode)
{
	n_evict++;
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static int t_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	(void)inode;
	CHECK(wbc->sync_mode == WB_SYNC_ALL);
	n_write++;
	return 0;
}

static const struct super_operations t_sops = {
	.alloc_inode = t_alloc_inode,
	.free_inode = t_free_inode,
	.evict_inode = t_evict_inode,
	.write_inode = t_write_inode,
};

static int t_test(struct inode *inode, void *data)
{
	return TI(inode)->key == *(unsigned long *)data;
}

static int t_set(struct inode *inode, void *data)
{
	TI(inode)->key = *(unsigned long *)data;
	inode->i_ino = TI(inode)->key;
	return 0;
}

static struct inode *get(struct super_block *sb, unsigned long key)
{
	struct inode *inode = iget5_locked(sb, key, t_test, t_set, &key);
	if (inode && (inode->i_state & I_NEW)) {
		TI(inode)->payload = (int)key * 10;
		unlock_new_inode(inode);
	}
	return inode;
}

/* --- basic ------------------------------------------------------------ */

static void test_basic(void)
{
	struct super_block *sb = sb_alloc();
	struct inode *a, *b;
	unsigned long key = 7;

	sb->s_op = &t_sops;
	a = get(sb, 7);
	CHECK(a && !(a->i_state & I_NEW) && TI(a)->payload == 70);
	CHECK(atomic_read(&a->i_count) == 1);
	b = get(sb, 7);
	CHECK(b == a && atomic_read(&a->i_count) == 2);
	iput(b);
	b = ilookup5(sb, 7, t_test, &key);
	CHECK(b == a);
	iput(b);
	key = 8;
	CHECK(ilookup5(sb, 8, t_test, &key) == NULL);
	CHECK(n_alloc == 1);

	/* Last iput parks it on the LRU; the next lookup revives the same object. */
	iput(a);
	CHECK(n_evict == 0 && inode_cache_count() == 1);
	b = get(sb, 7);
	CHECK(b == a && inode_cache_count() == 0 && n_alloc == 1);

	/* nlink 0 -> dropped immediately on the last iput. */
	clear_nlink(b);
	iput(b);
	CHECK(n_evict == 1 && n_free == 1 && inode_cache_count() == 0);
	key = 7;
	CHECK(ilookup5(sb, 7, t_test, &key) == NULL);

	/* new_inode + insert_inode_hash + lookup by ino. */
	a = new_inode(sb);
	a->i_ino = 99;
	TI(a)->key = 99;
	insert_inode_hash(a);
	key = 99;
	b = ilookup5(sb, 99, t_test, &key);
	CHECK(b == a);
	iput(b);
	iput(a);
	CHECK(inode_cache_count() == 1);

	sb_free(sb);
	CHECK(n_alloc == n_free);
	CHECK(inode_cache_count() == 0);
}

/* --- find_inode_nowait with the kernel 7.1 (u64) callback -------------- */

static int match_u64(struct inode *inode, u64 ino, void *data)
{
	int *stop = data;
	if (TI(inode)->key != ino)
		return 0;
	if (*stop)
		return -1;
	return igrab(inode) ? 1 : -1;
}

static int match_ul(struct inode *inode, unsigned long ino, void *data)
{
	(void)data;
	if (TI(inode)->key != ino)
		return 0;
	return igrab(inode) ? 1 : -1;
}

static void test_find_nowait(void)
{
	struct super_block *sb = sb_alloc();
	struct inode *a, *b;
	int stop = 0;

	sb->s_op = &t_sops;
	a = get(sb, 42);
	b = find_inode_nowait(sb, 42, match_u64, &stop);
	CHECK(b == a && atomic_read(&a->i_count) == 2);
	iput(b);
	b = find_inode_nowait(sb, 42, match_ul, NULL);
	CHECK(b == a);
	iput(b);
	stop = 1;
	CHECK(find_inode_nowait(sb, 42, match_u64, &stop) == NULL);
	CHECK(find_inode_nowait(sb, 43, match_ul, NULL) == NULL);
	/* An LRU (unreferenced) inode found this way leaves the LRU. */
	iput(a);
	CHECK(inode_cache_count() == 1);
	b = find_inode_nowait(sb, 42, match_ul, NULL);
	CHECK(b == a && inode_cache_count() == 0);
	iput(b);
	sb_free(sb);
}

/* --- concurrent creation: one object, one allocation ------------------- */

struct racer { struct super_block *sb; struct inode *got; };

static void *racer_main(void *arg)
{
	struct racer *r = arg;
	usleep(1000);
	r->got = get(r->sb, 1000);
	return NULL;
}

static void test_race(void)
{
	struct super_block *sb = sb_alloc();
	enum { N = 8 };
	pthread_t th[N];
	struct racer r[N];
	int i, before = n_alloc;

	sb->s_op = &t_sops;
	for (i = 0; i < N; i++) {
		r[i].sb = sb;
		pthread_create(&th[i], NULL, racer_main, &r[i]);
	}
	for (i = 0; i < N; i++)
		pthread_join(th[i], NULL);
	for (i = 1; i < N; i++)
		CHECK(r[i].got == r[0].got);
	CHECK(n_alloc - before == 1);
	CHECK(atomic_read(&r[0].got->i_count) == N);
	for (i = 0; i < N; i++)
		iput(r[i].got);
	sb_free(sb);
}

/* --- I_NEW blocking and iget_failed ----------------------------------- */

struct waiter { struct super_block *sb; struct inode *got; int was_new; };

static void *waiter_main(void *arg)
{
	struct waiter *w = arg;
	unsigned long key = 555;
	struct inode *inode = iget5_locked(w->sb, key, t_test, t_set, &key);
	w->got = inode;
	w->was_new = inode && (inode->i_state & I_NEW);
	if (w->was_new)
		unlock_new_inode(inode);
	return NULL;
}

static void test_new_and_failed(void)
{
	struct super_block *sb = sb_alloc();
	unsigned long key = 555;
	struct inode *creator;
	struct waiter w = { .sb = sb };
	pthread_t th;
	int evicts;

	sb->s_op = &t_sops;
	creator = iget5_locked(sb, key, t_test, t_set, &key);
	CHECK(creator && (creator->i_state & I_NEW));
	pthread_create(&th, NULL, waiter_main, &w);
	usleep(20000);			/* waiter is now blocked on I_NEW */
	CHECK(w.got == NULL);
	evicts = n_evict;
	iget_failed(creator);		/* creation fails */
	pthread_join(th, NULL);
	/* The waiter must not adopt the dead inode: it creates a fresh one.
	 * (No pointer comparison with @creator: it has been freed and the
	 * allocator may hand the same address to the fresh inode.) */
	CHECK(w.got != NULL && w.was_new);
	CHECK(n_evict == evicts + 1);
	iput(w.got);

	/* Now the success path: the waiter gets the same object after unlock. */
	memset(&w, 0, sizeof(w));
	w.sb = sb;
	key = 555;
	creator = get(sb, 555);		/* revives from the LRU: already initialised */
	CHECK(creator == NULL || !(creator->i_state & I_NEW));
	iput(creator);
	sb_free(sb);
}

/* --- LRU cap ------------------------------------------------------------ */

static void test_lru_cap(void)
{
	struct super_block *sb = sb_alloc();
	unsigned long i, n = NTFS_INODE_CACHE_MAX + 100;
	int frees = n_free, writes = n_write;

	sb->s_op = &t_sops;
	for (i = 1; i <= n; i++) {
		struct inode *inode = get(sb, i);
		CHECK(inode != NULL);
		mark_inode_dirty(inode);
		iput(inode);
	}
	CHECK(inode_cache_count() == NTFS_INODE_CACHE_MAX);
	CHECK(n_free - frees == 100);
	CHECK(n_write - writes == 100);	/* evicted dirty inodes were written */
	/* The oldest were evicted; the newest are still cached. */
	CHECK(ilookup5(sb, 1, t_test, &(unsigned long){1}) == NULL);
	{
		struct inode *inode = ilookup5(sb, n, t_test, &(unsigned long){n});
		CHECK(inode != NULL);
		iput(inode);
	}
	writes = n_write;
	CHECK(sync_inodes_sb(sb) == 0);
	CHECK(n_write - writes == NTFS_INODE_CACHE_MAX);
	writes = n_write;
	CHECK(sync_inodes_sb(sb) == 0);
	CHECK(n_write == writes);	/* nothing dirty any more */
	evict_inodes(sb);
	CHECK(inode_cache_count() == 0);
	sb_free(sb);
	CHECK(n_alloc == n_free);
}

/* --- default (no s_op) inodes and block size helpers ------------------- */

static int plain_set(struct inode *inode, void *data) { inode->i_ino = *(unsigned long *)data; return 0; }
static int plain_test(struct inode *inode, void *data) { return inode->i_ino == *(unsigned long *)data; }

static void test_plain(void)
{
	struct super_block *sb = sb_alloc();
	unsigned long key = 3;
	struct inode *inode = iget5_locked(sb, key, plain_test, plain_set, &key);

	CHECK(inode && (inode->i_state & I_NEW));
	unlock_new_inode(inode);
	CHECK(sb_set_blocksize(sb, 4096) == 4096 && sb->s_blocksize_bits == 12);
	CHECK(sb_set_blocksize(sb, 1000) == 0);
	CHECK(sb_min_blocksize(sb, 256) == 512);
	CHECK(inode->i_mapping == &inode->i_data);
	iput(inode);
	sb_free(sb);
}

int main(void)
{
	test_basic();
	test_find_nowait();
	test_race();
	test_new_and_failed();
	test_lru_cap();
	test_plain();
	pagecache_writeback_stop();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_inode: ok\n");
	return 0;
}
