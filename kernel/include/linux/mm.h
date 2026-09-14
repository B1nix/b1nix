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
/* A hint that this mapping would benefit from huge pages. There are none for
 * file mappings here, so it is recorded and not acted on. */
#define VM_HUGEPAGE  0x20000
/* MAP_SYNC: writes through the mapping are durable when the fault returns. */
#define VM_SYNC      0x00800000
/* Bytes read from the file are stable while mapped — what MAP_SYNC promises for
 * a DAX mapping. Declared so a filesystem can advertise it; nothing here can
 * satisfy it, and `mmap_supported_flags` is how the VFS refuses it. */
#define MAP_SYNC     0x080000

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
/*
 * `struct folio` mirrors `struct page` field for field.
 *
 * It is a separate type so that a folio pointer and a page pointer cannot be
 * mixed up — imported code holds both — and it has its own members rather than
 * embedding a page so that `folio->mapping` and `folio->index` work, which is
 * how upstream's imported code spells them.
 *
 * The two layouts MUST agree, because page_folio() and folio_page() are casts.
 * That is not left to inspection: FOLIO_MATCH below asserts every offset at
 * compile time, so a field added to one and not the other is a build error
 * rather than a pointer into the wrong word.
 */
struct folio {
	/*
	 * The union is upstream's, and it is not a space optimisation: imported
	 * code reaches BOTH ways. It writes `folio->mapping` and `folio->index`
	 * through the named fields, and it writes `&folio->page` to hand the
	 * folio to an interface that still takes a page — btrfs's compression
	 * path does exactly that. Only one of the two would compile; both are
	 * used.
	 */
	union {
		struct {
			u64 phys;
			volatile i32 count;
			u32 order;
			struct page *hash_next;
			/* A pointer here where struct page has an unsigned long, which
			 * is upstream's arrangement: the slot is the same word, and a
			 * folio's users store a pointer in it (iomap keeps its per-folio
			 * state there, ext4 its buffer heads). */
			void *private;
			struct list_head lru;
			struct address_space *mapping;
			unsigned long index;
			volatile unsigned long flags;
		};
		struct page page;
	};
};

/*
 * The two layouts MUST agree, because page_folio() and folio_page() are casts
 * and because the union above overlays them. That is not left to inspection:
 * every offset is asserted at compile time, so a field added to one and not the
 * other is a build error rather than a pointer into the wrong word.
 */
/* _Static_assert rather than static_assert: this header is included by
 * b1nix-side files that do not force-include <linux/compiler_types.h>, where
 * the one-argument spelling lives. */
#define FOLIO_MATCH(pfield, ffield)                                            \
	_Static_assert(offsetof(struct page, pfield) ==                            \
	                   offsetof(struct folio, ffield),                         \
	               "struct folio must mirror struct page: " #pfield)
FOLIO_MATCH(phys, phys);
FOLIO_MATCH(count, count);
FOLIO_MATCH(order, order);
FOLIO_MATCH(hash_next, hash_next);
FOLIO_MATCH(private, private);
FOLIO_MATCH(lru, lru);
FOLIO_MATCH(mapping, mapping);
FOLIO_MATCH(index, index);
FOLIO_MATCH(flags, flags);
_Static_assert(sizeof(struct page) == sizeof(struct folio),
               "struct folio and struct page must be the same size");
#undef FOLIO_MATCH

static inline struct page *folio_page(struct folio *f, usize n)
{ (void)n; return (struct page *)f; }
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

/*
 * Page flags.
 *
 * These are the state machine of the page cache, and each one is a promise:
 *
 *   Uptodate  the contents match what is on disk. A reader may use the bytes.
 *   Dirty     the contents differ and the DISK is the stale one.
 *   Locked    somebody owns the page; wait before touching it.
 *   Writeback an I/O is in flight writing it out.
 *   Private   `page->private` holds something — for ext4, its buffer heads.
 *
 * Uptodate and Dirty are independent: a freshly written page is both, a page
 * just read is Uptodate and clean, and a page being filled is neither. Code
 * that treats them as one state corrupts data in the case it did not think of.
 *
 * The bit NUMBERS are ours; nothing outside this kernel sees them.
 */
/* The bit numbering is in <lkpi/page.h>, next to the field it describes; the
 * accessors below are generated on top of it. */

/*
 * Generated rather than written out, for the same reason the buffer-head ones
 * are: three forms times twenty flags is sixty near-identical functions, and a
 * typo in one of them sets the wrong bit — which does not fail to compile and
 * does not fail at run time until a crash.
 *
 * The atomic forms are the default. `__SetPage*` is non-atomic and is only for
 * a page nobody else can see yet.
 */
#define PAGEFLAG(uname, lname)                                                 \
static inline int Page##uname(const struct page *page)                         \
{ return (page->flags & (1UL << PG_##lname)) != 0; }                           \
static inline void SetPage##uname(struct page *page)                           \
{ __atomic_fetch_or((unsigned long *)&page->flags, 1UL << PG_##lname,          \
                    __ATOMIC_SEQ_CST); }                                       \
static inline void ClearPage##uname(struct page *page)                         \
{ __atomic_fetch_and((unsigned long *)&page->flags, ~(1UL << PG_##lname),      \
                     __ATOMIC_SEQ_CST); }                                      \
static inline void __SetPage##uname(struct page *page)                         \
{ page->flags |= (1UL << PG_##lname); }                                        \
static inline void __ClearPage##uname(struct page *page)                       \
{ page->flags &= ~(1UL << PG_##lname); }                                       \
static inline int TestSetPage##uname(struct page *page)                        \
{ return (__atomic_fetch_or((unsigned long *)&page->flags,                     \
                            1UL << PG_##lname, __ATOMIC_SEQ_CST) &             \
          (1UL << PG_##lname)) != 0; }                                         \
static inline int TestClearPage##uname(struct page *page)                      \
{ return (__atomic_fetch_and((unsigned long *)&page->flags,                    \
                             ~(1UL << PG_##lname), __ATOMIC_SEQ_CST) &         \
          (1UL << PG_##lname)) != 0; }

PAGEFLAG(Locked, locked)
PAGEFLAG(Referenced, referenced)
PAGEFLAG(Uptodate, uptodate)
PAGEFLAG(Dirty, dirty)
PAGEFLAG(LRU, lru)
PAGEFLAG(Active, active)
PAGEFLAG(Workingset, workingset)
PAGEFLAG(Error, error)
PAGEFLAG(Private, private)
PAGEFLAG(Private2, private_2)
PAGEFLAG(Writeback, writeback)
PAGEFLAG(MappedToDisk, mappedtodisk)
PAGEFLAG(Reclaim, reclaim)
PAGEFLAG(Checked, checked)
PAGEFLAG(Reserved, reserved)
PAGEFLAG(SwapBacked, swapbacked)
PAGEFLAG(Unevictable, unevictable)

/* The folio spellings of the same flags. One page per folio here, so each is
 * the page form — but the names are kept apart because the types are. */
#define FOLIOFLAG(lname, fname)                                                \
static inline bool folio_test_##fname(struct folio *folio)                     \
{ return (folio->flags & (1UL << PG_##lname)) != 0; }                      \
static inline void folio_set_##fname(struct folio *folio)                      \
{ __atomic_fetch_or((unsigned long *)&folio->flags, 1UL << PG_##lname,     \
                    __ATOMIC_SEQ_CST); }                                       \
static inline void folio_clear_##fname(struct folio *folio)                    \
{ __atomic_fetch_and((unsigned long *)&folio->flags, ~(1UL << PG_##lname), \
                     __ATOMIC_SEQ_CST); }

FOLIOFLAG(locked, locked)
FOLIOFLAG(uptodate, uptodate)
FOLIOFLAG(dirty, dirty)
FOLIOFLAG(writeback, writeback)
FOLIOFLAG(private, private)
FOLIOFLAG(error, error)
FOLIOFLAG(referenced, referenced)
FOLIOFLAG(reclaim, reclaim)
FOLIOFLAG(checked, checked)
FOLIOFLAG(mappedtodisk, mappedtodisk)
FOLIOFLAG(workingset, workingset)
FOLIOFLAG(lru, lru)
FOLIOFLAG(active, active)
FOLIOFLAG(swapbacked, swapbacked)
FOLIOFLAG(unevictable, unevictable)
FOLIOFLAG(private_2, private_2)
FOLIOFLAG(owner_2, owner_2)
FOLIOFLAG(readahead, readahead)
FOLIOFLAG(dropbehind, dropbehind)

/* Where in its file a page sits, and how big it is. `folio_pos` is in BYTES and
 * `folio_index` in pages; the two differ by PAGE_SHIFT and mixing them up puts
 * an I/O 4096 times too far into the file. */
static inline unsigned long folio_index(struct folio *folio)
{ return folio->index; }
static inline loff_t folio_pos(struct folio *folio)
{ return (loff_t)folio->index << PAGE_SHIFT; }
static inline size_t folio_size(struct folio *folio)
{ (void)folio; return PAGE_SIZE; }
static inline struct address_space *folio_mapping(struct folio *folio)
{ return folio->mapping; }
/* A macro: struct address_space is not complete in this header. */
#define folio_inode(folio) ((folio)->mapping->host)
static inline unsigned long page_index(struct page *page)
{ return page->index; }
static inline loff_t page_offset(struct page *page)
{ return (loff_t)page->index << PAGE_SHIFT; }
static inline void *page_private(struct page *page)
{ return (void *)page->private; }
static inline void set_page_private(struct page *page, unsigned long v)
{ page->private = v; }
static inline void *folio_get_private(struct folio *folio)
{ return folio->private; }
static inline void folio_attach_private(struct folio *folio, void *data)
{
	folio->private = data;
	folio_set_private(folio);
}
static inline void *folio_detach_private(struct folio *folio)
{
	void *data = folio_get_private(folio);

	folio->private = NULL;
	folio_clear_private(folio);
	return data;
}

static inline void attach_page_private(struct page *page, void *data)
{
	get_page(page);
	set_page_private(page, (unsigned long)data);
	SetPagePrivate(page);
}

static inline void *detach_page_private(struct page *page)
{
	void *data = (void *)page_private(page);

	if (!PagePrivate(page))
		return NULL;
	ClearPagePrivate(page);
	set_page_private(page, 0);
	put_page(page);
	return data;
}

/* Where in its folio an address or offset sits, and how many filesystem blocks
 * a folio holds. One page per folio here, so the folio forms are the page
 * forms — spelled separately because the callers are counting different
 * things and the day a folio is more than a page they must differ. */
/*
 * A macro, not a function: callers pass either a file offset or a POINTER into
 * the folio — jbd2 passes `bh->b_data` — and a typed parameter would reject one
 * of the two.
 */
#define offset_in_folio(folio, p) \
	((unsigned long)(p) & (folio_size(folio) - 1))

/* One whole page, copied. The length is PAGE_SIZE exactly — a caller with its
 * own length wants memcpy. `clear_page` is the matching operation and is
 * declared further down, next to the page allocator. */
static inline void copy_page(void *to, const void *from)
{ __builtin_memcpy(to, from, PAGE_SIZE); }

void set_page_writeback(struct page *page);

/*
 * The page or folio behind a kernel virtual address.
 *
 * jbd2 uses it on the buffers it allocated with kmalloc, to find the folio a
 * journal descriptor block lives in. It only works for addresses inside the
 * direct map — which is where kmalloc's are — and answers NULL for anything
 * else rather than fabricating a page for a vmapped address.
 */
struct page *virt_to_page(const void *addr);
struct folio *virt_to_folio(const void *addr);

void memcpy_from_folio(char *to, struct folio *folio, size_t offset,
                       size_t len);
void memcpy_to_folio(struct folio *folio, size_t offset, const char *from,
                     size_t len);

void folio_mark_dirty(struct folio *f);
void folio_mark_accessed(struct folio *f);
void folio_end_writeback(struct folio *folio);
void folio_wait_writeback(struct folio *folio);
void folio_start_writeback(struct folio *folio);

static inline unsigned long folio_pfn(struct folio *f)
{ return (unsigned long)(page_to_phys(folio_page(f, 0)) >> PAGE_SHIFT); }

static inline struct page *folio_file_page(struct folio *f, unsigned long index)
{ (void)index; return folio_page(f, 0); }
static inline void folio_put(struct folio *f) { (void)f; }
static inline void *folio_address(struct folio *f) { return page_address(folio_page(f, 0)); }

/*
 * A short array of folios collected in one pass.
 *
 * The entries are folios, not pages: imported code indexes `fbatch.folios[i]`
 * and passes the result straight to folio_* functions. The batch holds
 * REFERENCES — `folio_batch_release` drops them — so a batch that is filled and
 * then forgotten leaks a reference per entry, and the pages are never
 * reclaimed.
 */
#define PAGEVEC_SIZE 15

struct folio_batch {
	unsigned char nr;
	unsigned char i;            /* next entry folio_batch_next() returns */
	bool percpu_pvec_drained;
	struct folio *folios[PAGEVEC_SIZE];
};

static inline void folio_batch_init(struct folio_batch *fb)
{
	fb->nr = 0;
	fb->i = 0;
	fb->percpu_pvec_drained = false;
}

static inline struct folio *folio_batch_next(struct folio_batch *fb)
{
	if (fb->i == fb->nr)
		return NULL;
	return fb->folios[fb->i++];
}

/* Returns the room left, so a caller can stop before the next add fails. */
static inline unsigned folio_batch_add(struct folio_batch *fb, struct folio *f)
{
	if (fb->nr < PAGEVEC_SIZE)
		fb->folios[fb->nr++] = f;
	return PAGEVEC_SIZE - fb->nr;
}

static inline unsigned folio_batch_count(struct folio_batch *fb)
{ return fb->nr; }

void __folio_batch_release(struct folio_batch *fb);
static inline void folio_batch_release(struct folio_batch *fb)
{
	if (fb->nr)
		__folio_batch_release(fb);
}

/*
 * Allocate several pages at once into a caller's array. Returns how many it
 * managed, which may be fewer than asked — every caller loops until it has
 * enough or gives up, and one that treated a short return as failure would
 * throw away pages it had been given.
 */
unsigned long alloc_pages_bulk_array(gfp_t gfp, unsigned long nr_pages,
                                     struct page **page_array);
/* Park briefly after a failed allocation, before trying again. */
void memalloc_retry_wait(gfp_t gfp_flags);

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
#define VM_FAULT_HWPOISON 0x0010
#define VM_FAULT_RETRY   0x0400
#define VM_FAULT_NOPAGE  0x0100
#define VM_FAULT_RETRY   0x0400


/* Mark a page as modified so the backing store knows to write it out. b1nix's
 * GEM pages have no backing store to write back to — they are freed, not paged
 * — so the mark has nothing to act on and the page is already where it will
 * stay. */
/*
 * Dirtying a page goes through the folio, which dispatches to the mapping's
 * own dirty_folio. These were no-ops, and a no-op here means a filesystem
 * never learns that anything changed: btrfs marks its metadata blocks dirty
 * with exactly this call.
 */
static inline void set_page_dirty(struct page *page)
{
	if (page)
		folio_mark_dirty(page_folio(page));
}

void lkpi_page_lock(struct page *page);
void lkpi_page_unlock(struct page *page);

static inline void set_page_dirty_lock(struct page *page)
{
	if (!page)
		return;
	lkpi_page_lock(page);
	folio_mark_dirty(page_folio(page));
	lkpi_page_unlock(page);
}
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
 * PageHighMem is always false: the whole of RAM is in the direct map, so no
 * page ever needs mapping in to be touched.
 *
 * PageWriteback, SetPageReclaim and ClearPageReclaim used to be stubs here as
 * well, from when a struct page had no flags word. They read and write the real
 * bits now (see PAGEFLAG above) — a filesystem waits on writeback and a stub
 * that always answered "not under writeback" would let a reader see a page
 * whose I/O had not finished.
 */
static inline int PageHighMem(const struct page *p) { (void)p; return 0; }
/* The raw reference count, which ext4 waits on directly while truncating: it
 * needs to know when it holds the ONLY reference. Named as upstream names the
 * member so that `&page->_refcount` compiles. */
#define _refcount count
/* A page that is part of a multi-page compound allocation. There are none here:
 * every page is its own folio (see the note on struct folio), so the answer is
 * always no — and that is what keeps the compound paths in imported code
 * unreachable rather than half-supported. */
static inline int PageCompound(const struct page *p) { (void)p; return 0; }
static inline int page_mapped(const struct page *p) { (void)p; return 0; }
int clear_page_dirty_for_io(struct page *p);

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

/* Whole-page allocation in the address-not-page spelling, and the zeroing
 * variant. `get_zeroed_page` is not an optimisation over allocate-then-memset:
 * callers rely on the page being zero before anything else can see it. */
unsigned long __get_free_pages(gfp_t gfp, unsigned int order);
unsigned long get_zeroed_page(gfp_t gfp);
void free_pages(unsigned long addr, unsigned int order);

/* Declared at file scope: <linux/bvec.h> is not reached from here, and a
 * struct first named inside a prototype is a type local to it — which then
 * refuses to match the real one at the definition. */
struct bio_vec;

/* A page mapped for the duration of one bvec entry. */
void *bvec_kmap_local(struct bio_vec *bvec);
void memzero_bvec(struct bio_vec *bvec);
void memcpy_page(struct page *dst_page, size_t dst_off, struct page *src_page,
                 size_t src_off, size_t len);

void dump_page(struct page *page, const char *reason);

/*
 * The shared page of zeros.
 *
 * One page, read-only, that everything needing zeroes can point at instead of
 * allocating. iomap hands it to the block layer for a write of a hole. It takes
 * an address argument upstream (some architectures have several); here there is
 * one and the argument is ignored.
 */
struct page *lkpi_zero_page(void);
#define ZERO_PAGE(vaddr) ({ (void)(vaddr); lkpi_zero_page(); })

/*
 * Where to place a mapping when the caller has no preference.
 *
 * Upstream's transparent-hugepage version aligns the result so a 2 MiB page can
 * back it. There are no huge pages for file mappings here, so a filesystem that
 * installs it gets the ordinary placement — which is correct, just not aligned.
 */
unsigned long thp_get_unmapped_area(struct file *filp, unsigned long addr,
                                    unsigned long len, unsigned long pgoff,
                                    unsigned long flags);

/* Read-ahead marker: this page is the one whose read should trigger the next
 * batch. It shares PG_reclaim upstream, which is safe because a page cannot be
 * both being reclaimed and a read-ahead marker. */
static inline int PageReadahead(const struct page *p)
{ return (p->flags & (1UL << PG_reclaim)) != 0; }
static inline void SetPageReadahead(struct page *p)
{ SetPageReclaim(p); }
static inline void ClearPageReadahead(struct page *p)
{ ClearPageReclaim(p); }

/* Assertions that print the offending page or folio. The object is what makes
 * them useful, so it is passed through to the report rather than dropped. */
#define VM_BUG_ON_PAGE(cond, page)   BUG_ON(cond)
#define VM_BUG_ON_FOLIO(cond, folio) BUG_ON(cond)
#define VM_WARN_ON_ONCE_FOLIO(cond, folio) WARN_ON(cond)
#define VM_WARN_ON_FOLIO(cond, folio)      WARN_ON(cond)

/* A folio is always one page here (see folio_nr_pages). */
static inline unsigned int folio_order(const struct folio *folio)
{ (void)folio; return 0; }
static inline unsigned int folio_shift(const struct folio *folio)
{ return PAGE_SHIFT + folio_order(folio); }
static inline int page_ref_count(const struct page *page)
{ return page->count; }
static inline int folio_ref_count(const struct folio *folio)
{ return folio->count; }
/* All of RAM is in the direct map: nothing is highmem, and a kmap of part of
 * a folio is never a partial mapping. */
static inline bool folio_test_highmem(const struct folio *folio)
{ (void)folio; return false; }
static inline bool folio_test_partial_kmap(const struct folio *folio)
{ (void)folio; return false; }

static inline struct page *phys_to_page(phys_addr_t pa)
{ return pfn_to_page((unsigned long)(pa >> PAGE_SHIFT)); }

/* Fill the NULL slots of `page_array` with fresh pages; returns how many
 * slots are now filled. The 6.14 name for alloc_pages_bulk_array. */
#define alloc_pages_bulk(gfp, nr_pages, page_array) \
	alloc_pages_bulk_array((gfp), (nr_pages), (page_array))

/* The direct-map address of a physical address; all of RAM is mapped. */
static inline void *phys_to_virt(phys_addr_t pa)
{ return (char *)page_address(phys_to_page(pa)) + (pa & ~PAGE_MASK); }


#endif
