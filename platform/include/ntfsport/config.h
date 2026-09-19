/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Build-time configuration for the user-space port of the Linux NTFS driver.
 */
#ifndef NTFSPORT_CONFIG_H
#define NTFSPORT_CONFIG_H

/*
 * Logical page size of the metadata page cache. This is the slot size of our
 * own cache, not the host MMU page. The driver requires it to be >= the
 * largest record it maps in one page (MFT record, index block). 12 (4 KiB)
 * is the configuration the driver is tested in upstream; 14 (16 KiB) is a
 * supported alternative for experiments.
 */
#ifndef NTFS_PAGE_SHIFT
#define NTFS_PAGE_SHIFT 12
#endif

/* Write-back policy: dirty metadata folios are flushed within this many ms. */
#ifndef NTFS_WRITEBACK_INTERVAL_MS
#define NTFS_WRITEBACK_INTERVAL_MS 1000
#endif

/* Upper bound on cached metadata folios per volume before reclaim kicks in. */
#ifndef NTFS_PAGECACHE_MAX_FOLIOS
#define NTFS_PAGECACHE_MAX_FOLIOS (64 * 1024)	/* 256 MiB at 4 KiB */
#endif

#endif /* NTFSPORT_CONFIG_H */
