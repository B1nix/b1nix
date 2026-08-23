/* module_alloc — the loadable-module virtual address region (M95).
 *
 * Modules live at 0xFFFFFFFFC0000000..+128 MiB, i.e. PML4 slot 511 / PDPT slot
 * 511 — the same top-level entry the kernel image itself occupies. Every
 * address space clones PML4[256..511] by value at creation time, so the page
 * tables installed here are shared by all of them: a module mapped while an
 * `insmod` process is current is just as visible to the net daemon or an
 * interrupt handler on another core.
 *
 * The allocator is a page bitmap, not the general heap: module text has to be
 * mapped executable and read-only independently of module data, which is only
 * possible with page-granular ownership.
 */

#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/module.h>
#include <b1nix/spinlock.h>
#include <b1nix/tlb.h>
#include <string.h>

#define MODULE_REGION_PAGES ((usize)(MODULE_REGION_SIZE / PAGE_SIZE))
#define MODULE_BITMAP_WORDS (MODULE_REGION_PAGES / 64)
#define MODULE_ALLOC_MAX 64

static u64 module_bitmap[MODULE_BITMAP_WORDS];

struct module_alloc_rec {
  u64 base;
  usize pages;
};

static struct module_alloc_rec module_allocs[MODULE_ALLOC_MAX];
static usize module_alloc_pages_used;
static spinlock_t module_alloc_lock = SPINLOCK_INIT;

static int bitmap_test(usize page) {
  return (module_bitmap[page / 64] >> (page % 64)) & 1ULL;
}

static void bitmap_set(usize page) {
  module_bitmap[page / 64] |= 1ULL << (page % 64);
}

static void bitmap_clear(usize page) {
  module_bitmap[page / 64] &= ~(1ULL << (page % 64));
}

/* Reserve `pages` consecutive free pages. Returns the first page index, or
 * MODULE_REGION_PAGES when the region cannot satisfy the request. Caller holds
 * module_alloc_lock. */
static usize bitmap_reserve(usize pages) {
  usize run = 0;
  for (usize i = 0; i < MODULE_REGION_PAGES; i++) {
    if (bitmap_test(i)) {
      run = 0;
      continue;
    }
    run++;
    if (run == pages) {
      usize start = i + 1 - pages;
      for (usize j = start; j < start + pages; j++)
        bitmap_set(j);
      return start;
    }
  }
  return MODULE_REGION_PAGES;
}

int module_region_contains(u64 addr) {
  return addr >= MODULE_REGION_BASE &&
         addr < MODULE_REGION_BASE + MODULE_REGION_SIZE;
}

usize module_alloc_used(void) { return module_alloc_pages_used * PAGE_SIZE; }

void *module_alloc(usize size) {
  if (size == 0)
    return 0;
  usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

  u64 flags;
  spin_lock_irqsave(&module_alloc_lock, &flags);
  usize slot = MODULE_ALLOC_MAX;
  for (usize i = 0; i < MODULE_ALLOC_MAX; i++) {
    if (module_allocs[i].pages == 0) {
      slot = i;
      break;
    }
  }
  if (slot == MODULE_ALLOC_MAX) {
    spin_unlock_irqrestore(&module_alloc_lock, flags);
    return 0;
  }
  usize start = bitmap_reserve(pages);
  if (start == MODULE_REGION_PAGES) {
    spin_unlock_irqrestore(&module_alloc_lock, flags);
    return 0;
  }
  u64 base = MODULE_REGION_BASE + (u64)start * PAGE_SIZE;
  module_allocs[slot].base = base;
  module_allocs[slot].pages = pages;
  module_alloc_pages_used += pages;
  spin_unlock_irqrestore(&module_alloc_lock, flags);

  /* Back the range with real frames, mapped RW+NX to start with: the loader
   * writes section contents and applies relocations before anything is made
   * executable. */
  usize mapped = 0;
  for (usize i = 0; i < pages; i++) {
#if defined(__aarch64__)
    /* Identity: the region IS reserved physical memory (b1nix/module.h), so a
     * page is backed by the frame at its own address. */
    u64 frame = base + (u64)i * PAGE_SIZE;
#else
    u64 frame = pmm_alloc_frame();
    if (!frame)
      break;
#endif
    vmm_map_page(base + (u64)i * PAGE_SIZE, frame,
                 VMM_PRESENT | VMM_WRITABLE | VMM_NO_EXECUTE);
    mapped++;
  }
  if (mapped != pages) {
    /* Out of physical memory: undo the partial mapping and the reservation. */
    for (usize i = 0; i < mapped; i++) {
      u64 va = base + (u64)i * PAGE_SIZE;
      u64 frame = vmm_virt_to_phys((void *)(usize)va);
      vmm_unmap_page(va);
#if !defined(__aarch64__)
      if (frame)
        pmm_free_frame(frame);
#else
      (void)frame; /* reserved region: the frames are not the pmm's to take back */
#endif
    }
    spin_lock_irqsave(&module_alloc_lock, &flags);
    for (usize j = start; j < start + pages; j++)
      bitmap_clear(j);
    module_allocs[slot].pages = 0;
    module_alloc_pages_used -= pages;
    spin_unlock_irqrestore(&module_alloc_lock, flags);
    return 0;
  }

  memset((void *)(usize)base, 0, pages * PAGE_SIZE);
  return (void *)(usize)base;
}

int module_set_prot(void *addr, usize size, int prot) {
  u64 va = (u64)(usize)addr;
  if (!module_region_contains(va))
    return -1;
  if ((va & (PAGE_SIZE - 1)) != 0)
    return -1;
  if (size == 0)
    return 0;

  u64 pte_flags = (prot == MODULE_PROT_RX)
                      ? VMM_PRESENT
                      : (VMM_PRESENT | VMM_WRITABLE | VMM_NO_EXECUTE);
  usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  for (usize i = 0; i < pages; i++) {
    u64 page = va + (u64)i * PAGE_SIZE;
    if (!module_region_contains(page))
      return -1;
    paging_mprotect_page(page, pte_flags);
    /* Kernel-half mapping: every CPU may have cached the old permissions. */
    tlb_shootdown_page(page);
  }
  return 0;
}

void module_free(void *addr) {
  u64 base = (u64)(usize)addr;
  if (!module_region_contains(base))
    return;

  u64 flags;
  spin_lock_irqsave(&module_alloc_lock, &flags);
  usize slot = MODULE_ALLOC_MAX;
  for (usize i = 0; i < MODULE_ALLOC_MAX; i++) {
    if (module_allocs[i].pages != 0 && module_allocs[i].base == base) {
      slot = i;
      break;
    }
  }
  if (slot == MODULE_ALLOC_MAX) {
    spin_unlock_irqrestore(&module_alloc_lock, flags);
    return;
  }
  usize pages = module_allocs[slot].pages;
  module_allocs[slot].pages = 0;
  usize start = (usize)((base - MODULE_REGION_BASE) / PAGE_SIZE);
  for (usize j = start; j < start + pages; j++)
    bitmap_clear(j);
  module_alloc_pages_used -= pages;
  spin_unlock_irqrestore(&module_alloc_lock, flags);

  for (usize i = 0; i < pages; i++) {
    u64 va = base + (u64)i * PAGE_SIZE;
    /* Restore write permission first: vmm_unmap_page reads the leaf PTE, and a
     * read-only text page is unmapped exactly the same way, but keeping the
     * two paths identical avoids surprises if that ever changes. */
    paging_mprotect_page(va, VMM_PRESENT | VMM_WRITABLE | VMM_NO_EXECUTE);
    u64 frame = vmm_virt_to_phys((void *)(usize)va);
    vmm_unmap_page(va);
    if (frame)
      pmm_free_frame(frame);
  }
}
