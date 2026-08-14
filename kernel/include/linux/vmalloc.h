/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_VMALLOC_H
#define LKPI_LINUX_VMALLOC_H
#include <linux/slab.h>
#include <lkpi/page.h>
/* Linux's vmalloc gives a linear range backed by scattered pages. b1nix's
 * kheap already hands back kernel-linear memory, so for a plain allocation
 * these are kmalloc; the scattered-pages case that actually needs stitching is
 * lkpi_vmap, and callers that mean that use it by name. */
static inline void *vmalloc(usize size) { return lkpi_kmalloc(size, GFP_KERNEL); }
static inline void *vzalloc(usize size)
{ return lkpi_kmalloc(size, GFP_KERNEL | __GFP_ZERO); }
static inline void vfree(const void *p) { lkpi_kfree((void *)p); }

/* Mapping a page array into one linear range, and taking it back down. Onto
 * lkpi's vmap window — the same one is_vmalloc_addr tests against. */
void *vmap(struct page **pages, unsigned int count, unsigned long flags,
           pgprot_t prot);
void vunmap(const void *addr);

#define VM_MAP 0x00000004


/*
 * Mapping a list of frame numbers, rather than struct pages, into one linear
 * range. This is how a driver maps device memory it has no pages for.
 *
 * Declared and deliberately not defined. lkpi's vmap window is built from
 * struct page — it takes each page's frame from the page itself. Handing it
 * bare PFNs would need the inverse lookup b1nix does not have (see
 * pfn_to_page() in <linux/mm.h>), so a caller fails to link rather than
 * mapping the wrong frames.
 */
void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot);


/* A notifier fired when the vmap arena is about to purge lazily-freed areas, so
 * a driver can drop its own mappings first. lkpi's vmap window frees eagerly —
 * there is no lazy list to purge — so nothing ever fires this and registering
 * is recorded nowhere. */
struct notifier_block;
static inline int register_vmap_purge_notifier(struct notifier_block *nb)
{ (void)nb; return 0; }
static inline int unregister_vmap_purge_notifier(struct notifier_block *nb)
{ (void)nb; return 0; }

#endif
