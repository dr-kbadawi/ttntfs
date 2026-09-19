/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_HIGHMEM_H
#define _LINUX_HIGHMEM_H

#include <linux/types.h>
#include <linux/mm_types.h>

/* Folio data is always directly addressable; mapping is pointer math. */
static inline void *kmap_local_folio(struct folio *folio, size_t offset) { return (char *)folio->data + offset; }
static inline void *kmap_local_page(struct page *page) { return page_folio(page)->data; }
static inline void *kmap(struct page *page) { return page_folio(page)->data; }
static inline void *kmap_atomic(struct page *page) { return page_folio(page)->data; }
static inline void kunmap_local(const void *addr) { (void)addr; }
static inline void kunmap(struct page *page) { (void)page; }
static inline void kunmap_atomic(void *addr) { (void)addr; }
static inline void *folio_map_local(struct folio *f) { return f->data; }

#endif /* _LINUX_HIGHMEM_H */
