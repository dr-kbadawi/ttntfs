/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SLAB_H
#define _LINUX_SLAB_H

#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <linux/compiler.h>

/* GFP flags are accepted and ignored: allocation in user space never
 * needs to reason about reclaim context. */
#define __GFP_HIGHMEM	0x02u
#define __GFP_ZERO	0x100u
#define __GFP_NOFAIL	0x8000u
#define __GFP_NOWARN	0x200u
#define __GFP_NORETRY	0x1000u
#define __GFP_FS	0x80u
#define __GFP_IO	0x40u
#define GFP_KERNEL	(__GFP_FS | __GFP_IO)
#define GFP_NOFS	(__GFP_IO)
#define GFP_NOIO	0u
#define GFP_ATOMIC	0u
#define GFP_HIGHUSER	(GFP_KERNEL | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE GFP_HIGHUSER

void *platform_malloc(size_t size, gfp_t gfp);
void platform_free(const void *p);

static inline void *kmalloc(size_t size, gfp_t gfp) { return platform_malloc(size, gfp); }
static inline void *kzalloc(size_t size, gfp_t gfp) { return platform_malloc(size, gfp | __GFP_ZERO); }
static inline void *kmalloc_array(size_t n, size_t size, gfp_t gfp)
{ size_t t; return __builtin_mul_overflow(n, size, &t) ? NULL : platform_malloc(t, gfp); }
static inline void *kcalloc(size_t n, size_t size, gfp_t gfp) { return kmalloc_array(n, size, gfp | __GFP_ZERO); }
void *krealloc(const void *p, size_t new_size, gfp_t gfp);
static inline void kfree(const void *p) { platform_free(p); }
static inline void kfree_sensitive(const void *p) { platform_free(p); }
static inline void *kmemdup(const void *src, size_t len, gfp_t gfp)
{ void *p = platform_malloc(len, gfp); if (p) memcpy(p, src, len); return p; }
char *kstrdup(const char *s, gfp_t gfp);
char *kstrndup(const char *s, size_t len, gfp_t gfp);

/* vmalloc family == malloc here. */
static inline void *vmalloc(unsigned long size) { return platform_malloc(size, GFP_KERNEL); }
static inline void *vzalloc(unsigned long size) { return platform_malloc(size, GFP_KERNEL | __GFP_ZERO); }
static inline void *__vmalloc(unsigned long size, gfp_t gfp) { return platform_malloc(size, gfp); }
static inline void vfree(const void *p) { platform_free(p); }
static inline void *kvmalloc(size_t size, gfp_t gfp) { return platform_malloc(size, gfp); }
static inline void *kvzalloc(size_t size, gfp_t gfp) { return platform_malloc(size, gfp | __GFP_ZERO); }
static inline void *kvmalloc_array(size_t n, size_t size, gfp_t gfp) { return kmalloc_array(n, size, gfp); }
static inline void *kvcalloc(size_t n, size_t size, gfp_t gfp) { return kcalloc(n, size, gfp); }
static inline void kvfree(const void *p) { platform_free(p); }
/* vmap of cache folios: returns a contiguous copy or a direct pointer;
 * implemented in platform/src/mem.c. */
struct page;
void *vmap(struct page **pages, unsigned int count, unsigned long flags, int prot);
void vunmap(const void *addr);
#define VM_MAP 0x4
#define PAGE_KERNEL 0
#define PAGE_KERNEL_RO 1
static inline void invalidate_kernel_vmap_range(void *addr, int size) { (void)addr; (void)size; }

/* Page allocation returns PAGE_SIZE-aligned memory. */
unsigned long __get_free_page(gfp_t gfp);
unsigned long __get_free_pages(gfp_t gfp, unsigned int order);
void free_page(unsigned long addr);
void free_pages(unsigned long addr, unsigned int order);
#define get_zeroed_page(gfp) __get_free_page((gfp) | __GFP_ZERO)

/* kmem_cache: constructor-aware allocator. */
struct kmem_cache;
#define SLAB_HWCACHE_ALIGN	0x2000u
#define SLAB_RECLAIM_ACCOUNT	0x20000u
#define SLAB_MEM_SPREAD		0x100000u
#define SLAB_ACCOUNT		0x4000000u
#define SLAB_PANIC		0x40000u
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align,
				     unsigned int flags, void (*ctor)(void *));
void kmem_cache_destroy(struct kmem_cache *s);
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t gfp);
static inline void *kmem_cache_zalloc(struct kmem_cache *s, gfp_t gfp) { return kmem_cache_alloc(s, gfp | __GFP_ZERO); }
void kmem_cache_free(struct kmem_cache *s, void *p);
#define KMEM_CACHE(__struct, __flags) kmem_cache_create(#__struct, sizeof(struct __struct), 0, (__flags), NULL)

/* memalloc scopes are no-ops. */
static inline unsigned int memalloc_nofs_save(void) { return 0; }
static inline void memalloc_nofs_restore(unsigned int f) { (void)f; }
static inline unsigned int memalloc_noio_save(void) { return 0; }
static inline void memalloc_noio_restore(unsigned int f) { (void)f; }

/* Cleanup helpers for __free(kfree). */
static inline void __cleanup_kfree(void *pp) { kfree(*(void **)pp); }

#endif /* _LINUX_SLAB_H */
