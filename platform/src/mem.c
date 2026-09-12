/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Memory for the user-space port: kmalloc family, page allocation,
 * kmem_cache with constructors, and vmap (linux/slab.h, linux/pagemap.h).
 *
 * Pages returned by alloc_page() are standalone folios allocated by the
 * page cache itself (folio_alloc_standalone: PAGE_SIZE-aligned data,
 * initialised lock, refcount 1, counted in its live-folio total) so
 * put_page()/folio_put() from platform/pagecache free them, as compress.c
 * does.
 *
 * vmap() cannot alias memory in user space (a folio is 4 KiB, the host page
 * 16 KiB), so it returns a contiguous copy. vunmap() of a writable mapping
 * copies the buffer back into the pages. Callers that need the pages'
 * contents while still mapped must read the mapped buffer (compress.c's
 * write path was ported to do exactly that).
 */
#include <stdlib.h>
#include <string.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/pagemap.h>

/* ------------------------------------------------------------------ */
/* kmalloc                                                             */

void *platform_malloc(size_t size, gfp_t gfp)
{
	void *p = malloc(size ? size : 1);
	if (p && (gfp & __GFP_ZERO))
		memset(p, 0, size);
	return p;
}

void platform_free(const void *p)
{
	free((void *)p);
}

void *krealloc(const void *p, size_t new_size, gfp_t gfp)
{
	size_t old_size = p ? malloc_size(p) : 0;
	void *n;

	if (!new_size) {
		free((void *)p);
		return NULL;
	}
	n = realloc((void *)p, new_size);
	if (n && (gfp & __GFP_ZERO) && new_size > old_size)
		memset((char *)n + old_size, 0, new_size - old_size);
	return n;
}

char *kstrdup(const char *s, gfp_t gfp)
{
	size_t len;
	char *p;

	if (!s)
		return NULL;
	len = strlen(s) + 1;
	p = platform_malloc(len, gfp);
	if (p)
		memcpy(p, s, len);
	return p;
}

char *kstrndup(const char *s, size_t max, gfp_t gfp)
{
	size_t len;
	char *p;

	if (!s)
		return NULL;
	len = strnlen(s, max);
	p = platform_malloc(len + 1, gfp);
	if (p) {
		memcpy(p, s, len);
		p[len] = '\0';
	}
	return p;
}

/* ------------------------------------------------------------------ */
/* Page-aligned allocations                                            */

static void *page_aligned_alloc(size_t size, gfp_t gfp)
{
	void *p = NULL;
	if (posix_memalign(&p, PAGE_SIZE, size) != 0)
		return NULL;
	if (gfp & __GFP_ZERO)
		memset(p, 0, size);
	return p;
}

unsigned long __get_free_pages(gfp_t gfp, unsigned int order)
{
	return (unsigned long)page_aligned_alloc(PAGE_SIZE << order, gfp);
}

unsigned long __get_free_page(gfp_t gfp)
{
	return __get_free_pages(gfp, 0);
}

void free_pages(unsigned long addr, unsigned int order)
{
	(void)order;
	free((void *)addr);
}

void free_page(unsigned long addr)
{
	free((void *)addr);
}

struct page *alloc_page(gfp_t gfp)
{
	/* Allocated by the page cache so its live-folio accounting (which
	 * folio_put() decrements on free) stays balanced. */
	return (struct page *)folio_alloc_standalone(gfp);
}

void __free_page(struct page *page)
{
	if (page)
		folio_put(page_folio(page));
}

/* ------------------------------------------------------------------ */
/* kmem_cache                                                          */

struct kmem_cache {
	char name[40];
	unsigned int size;
	unsigned int align;
	unsigned int flags;
	void (*ctor)(void *);
};

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align,
				     unsigned int flags, void (*ctor)(void *))
{
	struct kmem_cache *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	if (name)
		strlcpy(s->name, name, sizeof(s->name));
	s->size = size ? size : 1;
	if (flags & SLAB_HWCACHE_ALIGN && align < 64)
		align = 64;
	if (align < sizeof(void *))
		align = sizeof(void *);
	s->align = align;
	s->flags = flags;
	s->ctor = ctor;
	return s;
}

void kmem_cache_destroy(struct kmem_cache *s)
{
	free(s);
}

void *kmem_cache_alloc(struct kmem_cache *s, gfp_t gfp)
{
	void *p = NULL;

	if (!s)
		return NULL;
	if (s->align > 16) {
		if (posix_memalign(&p, s->align, round_up((size_t)s->size, s->align)) != 0)
			return NULL;
	} else {
		p = malloc(s->size);
		if (!p)
			return NULL;
	}
	/* Kernel semantics: a constructor initialises the object; zeroing
	 * on top of it would undo that, so __GFP_ZERO applies only without. */
	if (s->ctor)
		s->ctor(p);
	else if (gfp & __GFP_ZERO)
		memset(p, 0, s->size);
	return p;
}

void kmem_cache_free(struct kmem_cache *s, void *p)
{
	(void)s;
	free(p);
}

/* ------------------------------------------------------------------ */
/* vmap                                                                */

struct vmap_region {
	struct list_head link;
	void *buf;
	unsigned int count;
	int prot;
	struct page *pages[];
};

static LIST_HEAD(vmap_regions);
static pthread_mutex_t vmap_lock = PTHREAD_MUTEX_INITIALIZER;

void *vmap(struct page **pages, unsigned int count, unsigned long flags, int prot)
{
	struct vmap_region *r;
	unsigned int i;

	(void)flags;
	if (!count)
		return NULL;
	r = malloc(sizeof(*r) + (size_t)count * sizeof(struct page *));
	if (!r)
		return NULL;
	r->buf = page_aligned_alloc((size_t)count * PAGE_SIZE, 0);
	if (!r->buf) {
		free(r);
		return NULL;
	}
	r->count = count;
	r->prot = prot;
	for (i = 0; i < count; i++) {
		r->pages[i] = pages[i];
		if (pages[i])
			memcpy((char *)r->buf + (size_t)i * PAGE_SIZE, page_address(pages[i]), PAGE_SIZE);
		else
			memset((char *)r->buf + (size_t)i * PAGE_SIZE, 0, PAGE_SIZE);
	}
	pthread_mutex_lock(&vmap_lock);
	list_add(&r->link, &vmap_regions);
	pthread_mutex_unlock(&vmap_lock);
	return r->buf;
}

void vunmap(const void *addr)
{
	struct vmap_region *r, *found = NULL;
	unsigned int i;

	if (!addr)
		return;
	pthread_mutex_lock(&vmap_lock);
	list_for_each_entry(r, &vmap_regions, link) {
		if (r->buf == addr) {
			found = r;
			list_del(&r->link);
			break;
		}
	}
	pthread_mutex_unlock(&vmap_lock);
	if (!found) {
		WARN_ON(1);
		return;
	}
	if (found->prot != PAGE_KERNEL_RO)
		for (i = 0; i < found->count; i++)
			if (found->pages[i])
				memcpy(page_address(found->pages[i]),
				       (char *)found->buf + (size_t)i * PAGE_SIZE, PAGE_SIZE);
	free(found->buf);
	free(found);
}
