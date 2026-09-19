/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/* Shared fake backend for the page cache tests: an in-memory disk where
 * each inode owns a 16 MiB region selected by i_ino. */
#ifndef PC_FAKE_DISK_H
#define PC_FAKE_DISK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

#define DISK_SIZE (64u << 20)
#define REGION_SIZE (16u << 20)
#define REGION_PAGES (REGION_SIZE / PAGE_SIZE)

static unsigned char *fake_disk;
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic64_t fake_reads = ATOMIC64_INIT(0), fake_writes = ATOMIC64_INIT(0);
static int fake_fail_writes;	/* when set, write_folio returns -EIO */
static int fake_write_delay_us;	/* slow writes to widen races */
static int fake_redirty_left;	/* while > 0, write_folio re-dirties instead of writing */
/* Hosts of the mappings written, in order (first FAKE_ORDER_MAX only). */
#define FAKE_ORDER_MAX 64
static struct inode *fake_write_order[FAKE_ORDER_MAX];
static int fake_write_order_n;

static unsigned char *fake_addr(struct address_space *m, pgoff_t index)
{
	return fake_disk + (size_t)m->host->i_ino * REGION_SIZE + (size_t)index * PAGE_SIZE;
}

static int fake_read_folio(struct address_space *m, struct folio *f)
{
	if (f->index >= REGION_PAGES)
		return -EIO;
	atomic64_inc(&fake_reads);
	pthread_mutex_lock(&fake_lock);
	memcpy(f->data, fake_addr(m, f->index), PAGE_SIZE);
	pthread_mutex_unlock(&fake_lock);
	folio_mark_uptodate(f);
	return 0;
}

static int fake_write_folio(struct address_space *m, struct folio *f)
{
	if (f->index >= REGION_PAGES)
		return -EIO;
	if (__atomic_load_n(&fake_fail_writes, __ATOMIC_SEQ_CST))
		return -EIO;
	if (fake_write_delay_us)
		usleep(fake_write_delay_us);
	pthread_mutex_lock(&fake_lock);
	if (fake_redirty_left > 0) {
		/* Like ntfs_write_folio_resident() when mrec_lock is busy. */
		fake_redirty_left--;
		pthread_mutex_unlock(&fake_lock);
		folio_redirty_for_writepage(NULL, f);
		return 0;
	}
	atomic64_inc(&fake_writes);
	if (fake_write_order_n < FAKE_ORDER_MAX)
		fake_write_order[fake_write_order_n++] = m->host;
	memcpy(fake_addr(m, f->index), f->data, PAGE_SIZE);
	pthread_mutex_unlock(&fake_lock);
	return 0;
}

static const struct address_space_operations fake_aops = {
	.read_folio = fake_read_folio,
	.write_folio = fake_write_folio,
};

static struct super_block fake_sb;

static void fake_disk_init(void)
{
	size_t i;
	fake_disk = malloc(DISK_SIZE);
	for (i = 0; i < DISK_SIZE; i++)
		fake_disk[i] = (unsigned char)(i * 2654435761u >> 13);
}

static void fake_inode_init(struct inode *inode, unsigned long ino)
{
	memset(inode, 0, sizeof(*inode));
	inode->i_ino = ino;
	inode->i_sb = &fake_sb;
	inode->i_size = REGION_SIZE;
	inode->i_mapping = &inode->i_data;
	address_space_init(&inode->i_data, inode, &fake_aops);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#endif
