/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_MM_H
#define LKPI_LINUX_MM_H
#include <lkpi/page.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/types.h>
#include <lkpi/page.h>
/* Page constants and the page allocator, onto lkpi's struct page. There is no
 * global mem_map here, so nothing offers a physical-address-to-page lookup —
 * see <lkpi/page.h> for why and what that costs. */
#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#define PAGE_MASK  (~((u64)PAGE_SIZE - 1))
#define PFN_UP(x)   (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_DOWN(x) ((x) >> PAGE_SHIFT)
/* Flags on a userspace mapping. The numbering is b1nix's own; imported code
 * only ever ORs and tests them, never passes them to userspace. */
#define VM_READ      0x0001
#define VM_WRITE     0x0002
#define VM_EXEC      0x0004
#define VM_SHARED    0x0008
#define VM_IO        0x4000
#define VM_PFNMAP    0x0400
#define VM_DONTEXPAND 0x0040
#define VM_DONTDUMP  0x0080
#define VM_NORESERVE 0x0200
#define VM_MIXEDMAP  0x1000

#define PAGE_ALIGN(x)   ALIGN((x), PAGE_SIZE)
#define PAGE_ALIGNED(x) IS_ALIGNED((u64)(x), PAGE_SIZE)
#define offset_in_page(p) ((usize)(p) & (PAGE_SIZE - 1))
/* Tear down userspace mappings of a range so a fault re-enters the driver.
 * b1nix's VMA teardown is per-process and the DRM mmap path is not wired to it
 * yet; declared so the core compiles, implemented with that wiring. */
/* Move pages back onto the evictable lists after a driver un-pins them. b1nix's
 * reclaim does not track per-page evictability for driver pages, so there is no
 * list to move them to and nothing is pinned in the first place. */
static inline void check_move_unevictable_folios(void *fbatch) { (void)fbatch; }

/* A batch of folios collected while unpinning. b1nix has no folio layer; the
 * batch is a plain array here and releasing it drops the page references. */
/*
 * A folio is Linux's name for one or more physically contiguous pages managed
 * as a unit. b1nix has no such layer, so a folio here is exactly one page —
 * kept as its own type rather than an alias, because imported code holds both
 * kinds of pointer and mixing them would compile where it should not.
 */
struct folio { struct page page; };
static inline struct page *folio_page(struct folio *f, usize n)
{ (void)n; return &f->page; }
static inline usize folio_nr_pages(struct folio *f) { (void)f; return 1; }
/* The page within a folio that holds a given file index. One page per folio
 * here, so it is always that page. */
/* Physical frame number of a folio's first page. */
/* The folio a page belongs to. One page per folio here, so the page is its own
 * folio — and the cast is safe because struct folio starts with that page. */
static inline struct folio *page_folio(struct page *p)
{ return (struct folio *)p; }

/* Mark a folio's contents changed, so writeback knows to flush it. b1nix's
 * page cache tracks dirtiness on its own pages; a driver's anonymous pages have
 * no writeback path, so there is nothing to mark. */
/* Set flags on a mapping. A function rather than a plain OR because Linux made
 * vm_flags write-protected to catch callers modifying it outside mmap_lock;
 * keeping the spelling keeps that intent visible. */
/* Write-combining page protection, through the PAT setup M98 programmed. The
 * distinction from cached memory is real on a framebuffer: write-combining
 * turns a sequence of small writes into burst transfers. */
/* Page protection bits implied by a mapping's flags. */
static inline pgprot_t vm_get_page_prot(unsigned long vm_flags)
{
	u64 bits = 1ull; /* present */
	if (vm_flags & VM_WRITE)
		bits |= 1ull << 1;
	if (!(vm_flags & VM_EXEC))
		bits |= 1ull << 63;
	return __pgprot(bits);
}

/* Strip the memory-encryption bit from a mapping's protection. b1nix does not
 * enable SME/SEV — see <linux/mem_encrypt.h> — so there is no bit to strip. */
/* Length of a mapping in pages. */
static inline unsigned long vma_pages(struct vm_area_struct *vma)
{ return (vma->vm_end - vma->vm_start) >> PAGE_SHIFT; }

static inline pgprot_t pgprot_decrypted(pgprot_t prot) { return prot; }

static inline pgprot_t pgprot_writecombine(pgprot_t prot)
{ return __pgprot(pgprot_val(prot) | (1ull << 3) | (1ull << 7)); }
static inline pgprot_t pgprot_noncached(pgprot_t prot)
{ return __pgprot(pgprot_val(prot) | (1ull << 4) | (1ull << 3)); }

static inline void vm_flags_set(struct vm_area_struct *vma, unsigned long flags)
{ vma->vm_flags |= flags; }
static inline void vm_flags_clear(struct vm_area_struct *vma, unsigned long flags)
{ vma->vm_flags &= ~flags; }

static inline void folio_mark_dirty(struct folio *f) { (void)f; }
static inline void folio_mark_accessed(struct folio *f) { (void)f; }

static inline unsigned long folio_pfn(struct folio *f)
{ return (unsigned long)(page_to_phys(&f->page) >> PAGE_SHIFT); }

static inline struct page *folio_file_page(struct folio *f, unsigned long index)
{ (void)index; return &f->page; }
static inline void folio_put(struct folio *f) { (void)f; }
static inline void *folio_address(struct folio *f) { return page_address(&f->page); }

struct folio_batch { unsigned char nr; struct page *folios[15]; };
static inline void folio_batch_init(struct folio_batch *fb) { fb->nr = 0; }
static inline unsigned folio_batch_add(struct folio_batch *fb, struct folio *f)
{
	if (fb->nr < 15)
		fb->folios[fb->nr++] = &f->page;
	return 15 - fb->nr;
}

static inline void __folio_batch_release(struct folio_batch *fb) { fb->nr = 0; }
static inline unsigned folio_batch_count(struct folio_batch *fb)
{ return fb->nr; }
static inline void folio_batch_release(struct folio_batch *fb) { fb->nr = 0; }

/* NUMA node a device's memory should come from. One node here. */
#define dev_to_node(dev) (-1)

struct address_space;
void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin,
                         loff_t const holelen, int even_cows);

static inline void *page_to_virt(struct page *p) { return page_address(p); }
#endif
