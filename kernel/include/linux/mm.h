/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MM_H
#define LKPI_LINUX_MM_H
/* Upstream's <linux/mm.h> pulls the shrinker interface in, and drivers embed
 * a struct shrinker without including it themselves. Same chain here. */
#include <linux/shrinker.h>
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

/* struct page carries its frame, so this is a field read rather than a lookup
 * through a global mem_map — see <lkpi/page.h> for why there is no inverse. */
static inline unsigned long page_to_pfn(const struct page *page)
{ return (unsigned long)(page_to_phys(page) >> PAGE_SHIFT); }


/*
 * Frame number back to its struct page.
 *
 * Declared and deliberately not defined. b1nix has no global mem_map: a page is
 * allocated alongside its frame, so there is no table to look one up in — see
 * the note in <lkpi/page.h>, which states this as a property of the memory
 * model rather than a gap. Defining it to return NULL, or to fabricate a page,
 * would turn "this path cannot work here" into a null dereference somewhere
 * later. A caller that needs it fails to link, which is the honest outcome.
 */
struct page *pfn_to_page(unsigned long pfn);

/* Fault return codes, for a driver's ->fault handler. */
#define VM_FAULT_NOPAGE  0x0100
#define VM_FAULT_SIGBUS  0x0002
#define VM_FAULT_OOM     0x0001
#define VM_FAULT_RETRY   0x0400


/* Mark a page as modified so the backing store knows to write it out. b1nix's
 * GEM pages have no backing store to write back to — they are freed, not paged
 * — so the mark has nothing to act on and the page is already where it will
 * stay. */
static inline void set_page_dirty(struct page *page) { (void)page; }
static inline void set_page_dirty_lock(struct page *page) { (void)page; }
static inline void mark_page_accessed(struct page *page) { (void)page; }


/* Page access helpers travel with the memory interface upstream, and drivers
 * call kmap without including <linux/highmem.h> themselves. */
#include <linux/highmem.h>


/* Page-table attribute bits travel with the memory interface upstream. */
#include <linux/pgtable.h>


/* Is this address inside the vmap window? b1nix's vmap has one reserved range,
 * so the test is a range check rather than a tree lookup — and it is exact,
 * not a heuristic. */
bool is_vmalloc_addr(const void *x);


/*
 * Mapping a file into the calling process from kernel context.
 *
 * Declared and not defined. b1nix's mmap is a syscall path that works on the
 * current process's address space through its own VFS handle, and there is no
 * kernel-side entry that takes a struct file and returns a user address. A
 * driver reaching this — i915's mmap_offset ioctl fallback does — fails to link
 * rather than receiving an address in an address space nobody chose.
 */
unsigned long vm_mmap(struct file *file, unsigned long addr, unsigned long len,
                      unsigned long prot, unsigned long flag,
                      unsigned long offset);


/* Drivers reach vmap()/vfree() through <linux/mm.h> upstream, by way of the
 * chain that ends at <linux/vmalloc.h>. Same reach here. */
#include <linux/vmalloc.h>

/*
 * Page-state predicates and setters.
 *
 * b1nix's struct page has no flags word that reclaim consults: driver pages are
 * allocated and freed, never scanned. So these report the state that is always
 * true here rather than reading a bit — there is no high memory (the whole of
 * RAM is in the direct map), no writeback in flight for anonymous pages, and no
 * reclaim queue to hint at. What is lost is the hinting, not correctness: every
 * caller treats a false answer as "no special handling needed".
 */
static inline int PageHighMem(const struct page *p) { (void)p; return 0; }
static inline int PageWriteback(const struct page *p) { (void)p; return 0; }
static inline void SetPageReclaim(struct page *p) { (void)p; }
static inline void ClearPageReclaim(struct page *p) { (void)p; }
static inline int page_mapped(const struct page *p) { (void)p; return 0; }
static inline int clear_page_dirty_for_io(struct page *p) { (void)p; return 0; }

/* The n'th page after this one. Upstream has to go through the mem_map because
 * a page's neighbours in the array are its neighbours in memory; here a page
 * allocation of order > 0 hands back one struct page per frame in a contiguous
 * run, so pointer arithmetic is the same answer. */
static inline struct page *nth_page(struct page *p, unsigned long n)
{ return p + n; }

/* Whole-page allocation in the address-not-page spelling. */
unsigned long __get_free_page(gfp_t gfp);
void free_page(unsigned long addr);

/*
 * Walking another process's mappings.
 *
 * Declared and deliberately not defined. b1nix's VMAs live in its own
 * process-private structures and there is no kernel-side reader for another
 * task's address space — the userptr path is the only caller, and it must fail
 * to link rather than walk a list that is not the one the process is using.
 */
struct mm_struct;
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr);

/* The lock that walk is taken under. b1nix's fault path holds no lock a driver
 * could take or drop — an address space is only ever walked by the process that
 * owns it — so these bracket nothing. Taking them is not what is missing here;
 * find_vma() above is. */
static inline void mmap_read_lock(struct mm_struct *mm) { (void)mm; }
static inline void mmap_read_unlock(struct mm_struct *mm) { (void)mm; }
static inline int mmap_write_lock_killable(struct mm_struct *mm)
{ (void)mm; return 0; }
static inline void mmap_write_unlock(struct mm_struct *mm) { (void)mm; }

/* The iterator upstream uses to walk a range of VMAs. It holds the position;
 * the stepping is find_vma(), which is not defined here — see above. */
struct vma_iterator {
	struct mm_struct *mm;
	unsigned long addr;
};
#define VMA_ITERATOR(name, mm_, addr_) \
	struct vma_iterator name = { .mm = (mm_), .addr = (addr_) }
#define for_each_vma_range(vmi, vma, end)                                 \
	for ((vma) = find_vma((vmi).mm, (vmi).addr);                          \
	     (vma) && (vma)->vm_start < (end);                                \
	     (vmi).addr = (vma)->vm_end, (vma) = find_vma((vmi).mm, (vmi).addr))

/*
 * Dropping a range's PTEs so the next access faults back into the driver, and
 * walking a range's page tables from kernel context.
 *
 * Declared and not defined, for the same reason as vm_mmap() above: b1nix's
 * page tables are edited through its own VMM against a specific address space,
 * and there is no entry point that takes a foreign VMA. A caller fails to link
 * rather than silently editing nothing.
 */
int zap_vma_ptes(struct vm_area_struct *vma, unsigned long address,
                 unsigned long size);
typedef int (*pte_fn_t)(pte_t *pte, unsigned long addr, void *data);
int apply_to_page_range(struct mm_struct *mm, unsigned long address,
                        unsigned long size, pte_fn_t fn, void *data);

/* Point a VMA at the file that backs it, taking a reference. b1nix's VMAs carry
 * their backing handle from the mmap path itself, so there is nothing to
 * re-point; the reference upstream takes is the one the caller already holds. */
static inline void vma_set_file(struct vm_area_struct *vma, struct file *file)
{ (void)vma; (void)file; }

#define VM_MAYWRITE  0x00020000
#define VM_MAP_PUT_PAGES 0x00000100
#define FAULT_FLAG_RETRY_NOWAIT 0x08

/*
 * The reclaim-recursion lockdep annotation: upstream marks a region as "this
 * may be entered from reclaim" so lockdep can catch an allocation inside it.
 * b1nix has no lockdep, so there is no state to mark, and no diagnostic to
 * lose beyond the one lockdep would have given.
 */
static inline unsigned int fs_reclaim_acquire(gfp_t gfp) { (void)gfp; return 0; }
static inline void fs_reclaim_release(gfp_t gfp) { (void)gfp; }




/* Allocate on a given NUMA node. One node here, so the node is ignored and the
 * pages come from the only pool there is. */
#define alloc_pages_node(nid, gfp, order) ({ (void)(nid); alloc_pages(gfp, order); })

/* Zero a page, by its kernel address or through a temporary mapping. Every page
 * is in the direct map here, so the two are the same operation. */
static inline void clear_page(void *addr) { __builtin_memset(addr, 0, PAGE_SIZE); }
static inline void clear_highpage(struct page *page)
{ clear_page(page_address(page)); }
static inline void copy_highpage(struct page *to, struct page *from)
{ __builtin_memcpy(page_address(to), page_address(from), PAGE_SIZE); }

/* The page behind a direct-map address. Declared and not defined for the same
 * reason as pfn_to_page(): there is no reverse map from an address to its
 * struct page. */
struct page *virt_to_page(const void *addr);
struct page *vmalloc_to_page(const void *addr);

/* Should freed memory be poisoned? b1nix does not, so callers skip the extra
 * clear rather than doing it twice. */
static inline bool want_init_on_free(void) { return false; }

/* Is this a private mapping that will copy on write? b1nix's driver mappings
 * are all shared; a private one would break the "the GPU and the process see
 * the same page" invariant, so the answer is no and callers take the path that
 * assumes it. */
static inline bool is_cow_mapping(unsigned long flags)
{ return (flags & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE; }

/* May a fault handler drop the mmap lock and retry? b1nix's fault path holds no
 * lock it could drop, so it never asks the caller to come back. */
static inline bool fault_flag_allow_retry_first(unsigned int flags)
{ (void)flags; return false; }

#define VM_FAULT_ERROR (VM_FAULT_OOM | VM_FAULT_SIGBUS)

/*
 * Install a PFN mapping into a VMA from a fault handler.
 *
 * Declared and deliberately not defined, for the same reason as set_pte_at():
 * b1nix's page tables are edited through its own VMM against a named address
 * space, and there is no entry taking a foreign VMA and a raw frame.
 */
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
                               unsigned long pfn, pgprot_t pgprot);

/* Total and free memory, as /proc/meminfo reports it. */
struct sysinfo {
	unsigned long totalram;
	unsigned long freeram;
	unsigned long totalhigh;
	unsigned long freehigh;
	u32 mem_unit;
};
void si_meminfo(struct sysinfo *val);

/* CPU identification travels with the memory interface for the sources that
 * pick a copy strategy from it; ttm_module.c reads boot_cpu_data with only
 * <linux/mm.h> in scope. */
#include <linux/processor.h>

#endif
