/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MEMPOOL_H
#define LKPI_LINUX_MEMPOOL_H

#include <linux/types.h>
#include <linux/slab.h>

/*
 * A pool that guarantees an allocation can be satisfied.
 *
 * Upstream a mempool holds a reserve of pre-allocated elements so that a path
 * which MUST make progress — writing a page out in order to free memory — never
 * fails. `mempool_alloc` with __GFP_WAIT sleeps until an element is returned
 * rather than failing.
 *
 * There is no reserve here: b1nix's kmalloc does not block on memory pressure,
 * it panics when the heap cannot grow, so a reserve would protect against a
 * failure mode this allocator does not have. The pool is therefore the
 * allocator plus the caller's own alloc/free callbacks, and the guarantee it
 * makes is the allocator's guarantee.
 *
 * That difference is real and this is where it would have to change: a
 * kmalloc that could return NULL under pressure would make these paths fail
 * where upstream's cannot.
 */

typedef void *(mempool_alloc_t)(gfp_t gfp_mask, void *pool_data);
typedef void (mempool_free_t)(void *element, void *pool_data);

typedef struct mempool_s {
	int min_nr;
	int curr_nr;
	mempool_alloc_t *alloc;
	mempool_free_t *free;
	void *pool_data;
} mempool_t;

int mempool_init(mempool_t *pool, int min_nr, mempool_alloc_t *alloc_fn,
                 mempool_free_t *free_fn, void *pool_data);
void mempool_exit(mempool_t *pool);
mempool_t *mempool_create(int min_nr, mempool_alloc_t *alloc_fn,
                          mempool_free_t *free_fn, void *pool_data);
void mempool_destroy(mempool_t *pool);
void *mempool_alloc(mempool_t *pool, gfp_t gfp_mask);
void mempool_free(void *element, mempool_t *pool);

/* The stock callbacks: elements from a slab cache, and plain pages. */
void *mempool_alloc_slab(gfp_t gfp_mask, void *pool_data);
void mempool_free_slab(void *element, void *pool_data);
void *mempool_kmalloc(gfp_t gfp_mask, void *pool_data);
void mempool_kfree(void *element, void *pool_data);
void *mempool_alloc_pages(gfp_t gfp_mask, void *pool_data);
void mempool_free_pages(void *element, void *pool_data);

int mempool_init_slab_pool(mempool_t *pool, int min_nr, struct kmem_cache *kc);
int mempool_init_kmalloc_pool(mempool_t *pool, int min_nr, size_t size);
int mempool_init_page_pool(mempool_t *pool, int min_nr, int order);

mempool_t *mempool_create_slab_pool(int min_nr, struct kmem_cache *kc);
mempool_t *mempool_create_kmalloc_pool(int min_nr, size_t size);
mempool_t *mempool_create_page_pool(int min_nr, int order);

#endif
