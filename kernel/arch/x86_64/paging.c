#include <b1nix/console.h>
#include <b1nix/lockdep.h>
#include <b1nix/sched.h>
#include <b1nix/mm.h>
#include <b1nix/rwlock.h>
#include <b1nix/vfs.h>
#include <b1nix/page_cache.h>
#include <b1nix/panic.h>
#include <b1nix/klog.h>
#include <stdio.h>
#include <string.h>

/* F2 (M28 #7 prep): rwlock serialising page-table mutations across CPUs.
 * Today every kernel-mode entry holds the BKL so this is decorative; the
 * lock becomes load-bearing once the BKL teardown lands, at which point
 * two CPUs that both call vmm_map_page or vmm_unmap_page on the same
 * address space could race ensure_child_table / pt[idx] = entry and leak
 * an intermediate PT frame. Held as the writer for every map/unmap; the
 * page-fault read path stays lock-free for now (still BKL-protected).
 *
 * DAG position: HEAP (700) -> VMM (750) -> PMM (800). kheap_grow holds
 * heap_lock and calls vmm_map_page; the map path then allocates a PT
 * frame via pmm_alloc_frame. Reverse order is the kernel's normal
 * "kmalloc under vmm" pattern but never happens in practice — call sites
 * that need both lock VMM-first. */
static rwlock_t vmm_lock = RWLOCK_INIT;

static inline void vmm_write_acquire(u64 *flags) {
  rw_write_lock_irqsave(&vmm_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_VMM);
}
static inline void vmm_write_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_VMM);
  rw_write_unlock_irqrestore(&vmm_lock, flags);
}

static inline void vmm_read_acquire(u64 *flags) {
  rw_read_lock_irqsave(&vmm_lock, flags);
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_VMM);
}
static inline void vmm_read_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_VMM);
  rw_read_unlock_irqrestore(&vmm_lock, flags);
}

#define PAGE_ENTRY_ADDRESS_MASK 0x000ffffffffff000ULL
#define PAGE_TABLE_INDEX_MASK 0x1ffULL
#define HUGE_PAGE_FLAG (1ULL << 7)
/* DIRECT_MAP_SIZE is shared with the pmm via <b1nix/mm.h> (the pmm clamps
 * usable RAM to it so no frame is ever allocated outside the direct map). */
#define MMIO_MAP_BASE 0xffffa00000000000ULL
#define MMIO_MAP_SIZE (512ULL * 1024ULL * 1024ULL)

static u64 *kernel_pml4_virt;
static u64 kernel_pml4_phys;
int direct_map_ready;
static u64 mmio_next = MMIO_MAP_BASE;


static u64 *get_current_pml4(void) {
  if (current_task && current_task->pml4_phys) {
    return (u64 *)(usize)(current_task->pml4_phys + DIRECT_MAP_BASE);
  }
  return kernel_pml4_virt;
}

static inline int is_canonical(u64 addr) {
  return ((isize)addr >> 47) == 0 || ((isize)addr >> 47) == -1;
}

static u64 read_cr3(void) {
  u64 value;

  __asm__ volatile("movq %%cr3, %0" : "=r"(value));
  return value;
}

static void write_cr3(u64 value) {
  __asm__ volatile("movq %0, %%cr3" : : "r"(value) : "memory");
}

void paging_switch_address_space(u64 pml4_phys) {
  u64 target_phys = pml4_phys ? pml4_phys : kernel_pml4_phys;
  __asm__ volatile("mov %0, %%cr3" : : "r"(target_phys) : "memory");
}

static void invalidate_page(u64 virtual_address) {
  __asm__ volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

static usize pml4_index(u64 virtual_address) {
  return (virtual_address >> 39) & PAGE_TABLE_INDEX_MASK;
}

static usize pdpt_index(u64 virtual_address) {
  return (virtual_address >> 30) & PAGE_TABLE_INDEX_MASK;
}

static usize pd_index(u64 virtual_address) {
  return (virtual_address >> 21) & PAGE_TABLE_INDEX_MASK;
}

static usize pt_index(u64 virtual_address) {
  return (virtual_address >> 12) & PAGE_TABLE_INDEX_MASK;
}

static u64 *table_from_entry(u64 entry) {
  u64 phys = entry & PAGE_ENTRY_ADDRESS_MASK;
  if (direct_map_ready && phys < DIRECT_MAP_SIZE) {
    return (u64 *)(usize)(phys + DIRECT_MAP_BASE);
  }
  return (u64 *)(usize)phys;
}

static u64 table_to_phys(u64 *table) {
  u64 phys = (u64)(usize)table;
  if (phys >= DIRECT_MAP_BASE)
    phys -= DIRECT_MAP_BASE;
  return phys;
}

static u64 *alloc_page_table(void) {
  u64 frame = pmm_alloc_frame();
  if (frame == 0) {
    panic("vmm: OOM during page table allocation");
  }

  /* Page tables are dereferenced via the direct map once it is ready
   * (frame + DIRECT_MAP_BASE), so any frame below DIRECT_MAP_SIZE is
   * reachable. Before the direct map exists, only the low 4GB identity
   * window set up by the bootstrap (boot.S) is addressable. */
  u64 reachable_limit = direct_map_ready ? DIRECT_MAP_SIZE : 0x100000000ULL;
  if (frame >= reachable_limit) {
    panic("vmm: page table frame beyond reachable map");
  }

  u64 *table = (u64 *)(usize)frame;
  if (direct_map_ready && frame < DIRECT_MAP_SIZE) {
    table = (u64 *)(usize)(frame + DIRECT_MAP_BASE);
  }

  memset(table, 0, PAGE_SIZE);
  return table;
}

static u64 *split_huge_page(u64 *pd, usize index) {
  u64 entry = pd[index];
  u64 base = entry & PAGE_ENTRY_ADDRESS_MASK;
  u64 flags = entry & ~PAGE_ENTRY_ADDRESS_MASK;
  flags &= ~HUGE_PAGE_FLAG;

  /* The 2MB huge page entry in pd may have had VMM_USER set before split.
   * Strip VMM_USER when creating the 512 4KB leaf PTEs so the 511 identity-mapped
   * neighbor PTEs remain supervisor-only. The caller (vmm_map_page_locked /
   * vmm_set_lazy) will explicitly add VMM_USER to the single leaf PTE it maps.
   * This prevents free_table() during process teardown from treating identity-mapped
   * physical memory frames as user allocations and wrongly freeing them to PMM. */
  u64 leaf_flags = flags & ~VMM_USER;

  u64 *pt = alloc_page_table();
  for (usize i = 0; i < 512; i++) {
    pt[i] = (base + i * PAGE_SIZE) | leaf_flags;
  }

  pd[index] = table_to_phys(pt) | flags;
  return pt;
}

static u64 *ensure_child_table(u64 *parent, usize index) {
  if (!parent || (direct_map_ready && (u64)parent < DIRECT_MAP_BASE) || !is_canonical((u64)parent)) {
    console_write("ensure_child_table: INVALID parent: 0x");
    console_write_hex64((u64)parent);
    console_write(" index: ");
    console_write_hex64(index);
    console_write(" caller: 0x");
    console_write_hex64((u64)__builtin_return_address(0));
    console_write("\n");
    if (current_task) {
      console_write("current_task: ");
      console_write(current_task->name);
      console_write(" (id: ");
      console_write_dec(current_task->id);
      console_write(")\n");
    } else {
      console_write("current_task is NULL\n");
    }
    panic("ensure_child_table: invalid parent pointer");
  }
  u64 *result;
  if ((parent[index] & VMM_PRESENT) == 0) {
    u64 *child = alloc_page_table();
    parent[index] = table_to_phys(child) | VMM_PRESENT | VMM_WRITABLE;
    result = child;
  } else if ((parent[index] & HUGE_PAGE_FLAG) != 0) {
    result = split_huge_page(parent, index);
  } else {
    result = table_from_entry(parent[index]);
  }

  if (direct_map_ready && ((u64)result < DIRECT_MAP_BASE || !is_canonical((u64)result))) {
    console_write("ensure_child_table: returning INVALID pointer: 0x");
    console_write_hex64((u64)result);
    console_write(" parent: 0x");
    console_write_hex64((u64)parent);
    console_write(" index: ");
    console_write_hex64(index);
    console_write(" entry: 0x");
    console_write_hex64(parent[index]);
    console_write("\n");
  }
  return result;
}

void vmm_map_page_in_table(u64 *pml4, u64 virtual_address, u64 physical_address, u64 flags) {
  u64 *pdpt = ensure_child_table(pml4, pml4_index(virtual_address));
  u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual_address));

  /* DIAG: first time we map into the low-4GB on a user address space, dump
   * the PD entry to see if it's a 2MB identity-map huge page or already split. */
  {
    static int diag_done = 0;
    if (!diag_done && virtual_address >= 0x2000000ULL && virtual_address < 0x2100000ULL) {
      u64 pde_val = pd[pd_index(virtual_address)];
      console_write("MAP_IN_TABLE va=0x"); console_write_hex64(virtual_address);
      console_write(" pd["); console_write_dec((u32)pd_index(virtual_address));
      console_write("]=0x"); console_write_hex64(pde_val);
      console_write(pde_val & HUGE_PAGE_FLAG ? " HUGE" : " 4k");
      console_write(" flags=0x"); console_write_hex64(flags);
      console_write(" pml4="); console_write_hex64((u64)pml4);
      console_write("\n");
      diag_done = 1;
    }
  }

  u64 *pt;
  if ((pd[pd_index(virtual_address)] & HUGE_PAGE_FLAG) != 0) {
    pt = split_huge_page(pd, pd_index(virtual_address));
  } else {
    pt = ensure_child_table(pd, pd_index(virtual_address));
  }

  pt[pt_index(virtual_address)] =
      (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
  invalidate_page(virtual_address);
}

void vmm_init(void) {
  u64 phys_pml4 = pmm_alloc_frame();
  u64 *pml4 = (u64 *)(usize)phys_pml4;
  memset(pml4, 0, PAGE_SIZE);

  /* Map higher half (direct map) AND identity map using huge pages (2MB) */
  for (u64 physical = 0; physical < DIRECT_MAP_SIZE; physical += 0x200000ULL) {
    u64 virtual_high = DIRECT_MAP_BASE + physical;

    /* Higher-half mapping */
    u64 *pdpt_h = ensure_child_table(pml4, pml4_index(virtual_high));
    u64 *pd_h = ensure_child_table(pdpt_h, pdpt_index(virtual_high));
    pd_h[pd_index(virtual_high)] = physical | VMM_PRESENT | VMM_WRITABLE | (1ULL << 7);

    /* Identity mapping (for transition and physical access) */
    u64 *pdpt_i = ensure_child_table(pml4, pml4_index(physical));
    u64 *pd_i = ensure_child_table(pdpt_i, pdpt_index(physical));
    pd_i[pd_index(physical)] = physical | VMM_PRESENT | VMM_WRITABLE | (1ULL << 7);
  }

  /* Higher-half kernel window: map KERNEL_VMA (0xFFFFFFFF80000000) -> phys 0..
   * so the kernel — which is LINKED at KERNEL_VMA — stays mapped after we switch
   * CR3 to this pml4 below. Mirrors boot.S (pdpt[510] -> pd covering phys
   * 0-1GiB). 1 GiB is ample for the kernel image; this window is also what keeps
   * the kernel's data out of the low VA range userspace maps into. */
  for (u64 physical = 0; physical < 0x40000000ULL; physical += 0x200000ULL) {
    u64 virt = KERNEL_VMA + physical;
    u64 *pdpt_k = ensure_child_table(pml4, pml4_index(virt));
    u64 *pd_k = ensure_child_table(pdpt_k, pdpt_index(virt));
    pd_k[pd_index(virt)] = physical | VMM_PRESENT | VMM_WRITABLE | (1ULL << 7);
  }

  /* Pre-allocate page tables for the kernel heap in the higher half */
  u64 *pdpt_kheap = ensure_child_table(pml4, pml4_index(KHEAP_START));
  ensure_child_table(pdpt_kheap, pdpt_index(KHEAP_START));

  console_write("vmm: direct map 0x");
  console_write_hex64(DIRECT_MAP_BASE);
  console_write("-0x");
  console_write_hex64(DIRECT_MAP_BASE + DIRECT_MAP_SIZE);
  console_write("\n");

  /* Switch to new page table */
  paging_switch_address_space(phys_pml4);

  /* Now that direct map is ready, we can use virtual addresses for the PML4 */
  direct_map_ready = 1;
  kernel_pml4_virt = (u64 *)(usize)(phys_pml4 + DIRECT_MAP_BASE);
  kernel_pml4_phys = phys_pml4;

  extern void pmm_switch_to_direct_map(void);
  pmm_switch_to_direct_map();
}

/* Build the page-table path for virtual_address and install the leaf PTE.
 * Caller MUST already hold vmm_lock (write). No allocation here blocks
 * (alloc_page_table uses the non-blocking pmm fast path), so this is safe in an
 * IRQs-off critical section. Split out of vmm_map_page so the page-fault
 * handler — which also holds vmm_lock at commit time — can install without
 * re-entering the non-recursive rwlock. */
static void vmm_map_page_locked(u64 virtual_address, u64 physical_address,
                                u64 flags) {
  u64 *pml4 = get_current_pml4();
  u64 *pdpt = ensure_child_table(pml4, pml4_index(virtual_address));
  u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual_address));
  if ((flags & VMM_USER) != 0) {
    pml4[pml4_index(virtual_address)] |= VMM_USER;
    pdpt[pdpt_index(virtual_address)] |= VMM_USER;
  }

  u64 *pt;
  if ((pd[pd_index(virtual_address)] & HUGE_PAGE_FLAG) != 0) {
    pt = split_huge_page(pd, pd_index(virtual_address));
  } else {
    pt = ensure_child_table(pd, pd_index(virtual_address));
  }
  if ((flags & VMM_USER) != 0) {
    pd[pd_index(virtual_address)] |= VMM_USER;
  }
  pt[pt_index(virtual_address)] =
      (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
  invalidate_page(virtual_address);
}

void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0 ||
      (physical_address & (PAGE_SIZE - 1)) != 0) {
    panic("vmm_map_page requires page-aligned addresses");
  }

  if (kernel_pml4_virt == 0) {
    panic("vmm_map_page called before vmm_init");
  }

  u64 _vmflags;
  vmm_write_acquire(&_vmflags);
  vmm_map_page_locked(virtual_address, physical_address, flags);
  vmm_write_release(_vmflags);

  if ((flags & VMM_USER) && (flags & VMM_PRESENT)) {
    extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
    eviction_register_page(current_task, virtual_address, physical_address);
  }
}

void *vmm_map_mmio(u64 physical_address, usize size, u64 flags) {
  if (size == 0) {
    return 0;
  }

  u64 phys_base = physical_address & ~(PAGE_SIZE - 1);
  u64 phys_offset = physical_address - phys_base;
  u64 total_size = (u64)size + phys_offset;
  u64 map_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  u64 virt_base = (mmio_next + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (virt_base < MMIO_MAP_BASE ||
      virt_base + map_size > MMIO_MAP_BASE + MMIO_MAP_SIZE) {
    panic("vmm_map_mmio exhausted mmio virtual range");
  }

  for (u64 off = 0; off < map_size; off += PAGE_SIZE) {
    vmm_map_page(virt_base + off, phys_base + off, flags | VMM_PRESENT);
  }

  mmio_next = virt_base + map_size;
  return (void *)(usize)(virt_base + phys_offset);
}

static void unmap_page_from_pml4(u64 *pml4, u64 virtual_address) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0) {
    panic("vmm_unmap_page requires page-aligned address");
  }

  u64 pml4e = pml4[pml4_index(virtual_address)];
  if ((pml4e & VMM_PRESENT) == 0) {
    return;
  }

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  if ((pdpte & VMM_PRESENT) == 0) {
    return;
  }

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  if ((pde & VMM_PRESENT) == 0) {
    return;
  }

  if ((pde & HUGE_PAGE_FLAG) != 0) {
    pd[pd_index(virtual_address)] = 0;
    invalidate_page(virtual_address);
    return;
  }

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(virtual_address)];
  if (pte & VMM_PRESENT) {
    u64 frame = pte & PAGE_ENTRY_ADDRESS_MASK;
    if (frame) {
      if (pte & VMM_USER) {
        pmm_free_frame(frame);
        extern void eviction_unregister_page(u64 frame);
        eviction_unregister_page(frame);
      }
    }
  } else if (pte & VMM_SWAPPED) {
    /* The page is swapped out — release its swap slot (the slot index is encoded
     * in the PTE address field). Done here, incrementally per unmapped page, so
     * address-space teardown (the VMA-unmap loop) and munmap of a swapped range
     * both reclaim slots without any separate page-table walk. */
    extern void swap_free_slot_index(u32 slot);
    swap_free_slot_index((u32)((pte & PAGE_ENTRY_ADDRESS_MASK) >> 12));
  }
  pt[pt_index(virtual_address)] = 0;
  invalidate_page(virtual_address);
}

void vmm_unmap_page(u64 virtual_address) {
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);
  unmap_page_from_pml4(get_current_pml4(), virtual_address);
  vmm_write_release(_vmflags);
  /* M28 #5: every other CPU that has cached this translation needs to drop
   * it before we return; otherwise a write through their stale TLB entry hits
   * the (potentially freed-and-reused) physical frame. tlb_shootdown_page is
   * a no-op when g_max_cpus <= 1 so single-CPU boots pay nothing. Issued
   * with vmm_lock released so a target CPU that just took vmm_lock as a
   * reader doesn't block the shootdown ACK. */
  extern void tlb_shootdown_page(u64);
  tlb_shootdown_page(virtual_address);
}

void paging_unmap_page_from_space(u64 pml4_phys, u64 virtual_address) {
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);
  u64 *pml4 = pml4_phys ? (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE)
                         : kernel_pml4_virt;
  unmap_page_from_pml4(pml4, virtual_address);
  vmm_write_release(_vmflags);
}

void paging_map_page(u64 virtual_address, u64 physical_address, u64 flags) {
  vmm_map_page(virtual_address, physical_address, flags);
}

void paging_unmap_page(u64 virtual_address) { vmm_unmap_page(virtual_address); }

void paging_mprotect_page(u64 virtual_address, u64 flags) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0) {
    panic("paging_mprotect_page requires page-aligned address");
  }

  u64 *pml4 = get_current_pml4();
  u64 pml4e = pml4[pml4_index(virtual_address)];
  if ((pml4e & VMM_PRESENT) == 0)
    return;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  if ((pdpte & VMM_PRESENT) == 0)
    return;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  if ((pde & VMM_PRESENT) == 0)
    return;

  if ((pde & HUGE_PAGE_FLAG) != 0)
    return;

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(virtual_address)];

  if (pte & VMM_PRESENT) {
    u64 nf = flags;
    /* The shared zero page must never become directly writable through
     * mprotect — even after a PROT_READ downgrade dropped its COW marker — or
     * a store would corrupt the one frame mapped into every address space.
     * Re-arm COW so the first write materialises a private page. */
    if ((nf & VMM_WRITABLE) &&
        (pte & PAGE_ENTRY_ADDRESS_MASK) == pmm_zero_page()) {
      nf &= ~VMM_WRITABLE;
      nf |= VMM_COW;
    }
    /* Never let mprotect(PROT_WRITE) flip a COW page (fork-shared anon, or a
     * MAP_PRIVATE view of a page-cache frame) to directly-writable — that
     * would let stores corrupt the shared frame. Keep the COW marker and
     * leave the page read-only; the next store faults into the COW copy
     * path, which duplicates the frame and grants write access. */
    if ((pte & VMM_COW) && (nf & VMM_WRITABLE)) {
      nf &= ~VMM_WRITABLE;
      nf |= VMM_COW; /* write still faults into the COW copy path. A downgrade
                        to read-only (e.g. relro) intentionally drops the COW
                        marker: a later write is then a clean protection fault
                        (VMA prot tracking is too coarse to consult here). */
    }
    pt[pt_index(virtual_address)] =
        (pte & PAGE_ENTRY_ADDRESS_MASK) | nf | VMM_PRESENT;
  } else if (pte & (VMM_LAZY | VMM_SWAPPED)) {
    // For non-present pages, update the saved flags
    pt[pt_index(virtual_address)] =
        (pte & (PAGE_ENTRY_ADDRESS_MASK | VMM_LAZY | VMM_SWAPPED)) | flags;
  }

  invalidate_page(virtual_address);
}

/* Diagnostic: return the raw leaf PTE for a user VA in the current address
 * space, plus the covering PDE and whether that PDE is a 2 MiB huge page.
 * Returns 0 if any level is absent. Read-only, no allocation. */
u64 paging_debug_leaf_pte(u64 va, u64 *out_pde, int *out_huge) {
  if (out_pde) *out_pde = 0;
  if (out_huge) *out_huge = 0;
  u64 *pml4 = get_current_pml4();
  if (!pml4) return 0;
  u64 pml4e = pml4[pml4_index(va)];
  if ((pml4e & VMM_PRESENT) == 0) return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(va)];
  if ((pdpte & VMM_PRESENT) == 0) return 0;
  if (pdpte & HUGE_PAGE_FLAG) { if (out_huge) *out_huge = 1; if (out_pde) *out_pde = pdpte; return pdpte; }
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(va)];
  if (out_pde) *out_pde = pde;
  if ((pde & VMM_PRESENT) == 0) return 0;
  if (pde & HUGE_PAGE_FLAG) { if (out_huge) *out_huge = 1; return pde; }
  u64 *pt = table_from_entry(pde);
  return pt[pt_index(va)];
}

void vmm_remap_page(u64 virtual_address, u64 physical_address, u64 flags) {
  vmm_unmap_page(virtual_address);
  vmm_map_page(virtual_address, physical_address, flags);
}

u64 vmm_direct_map_base(void) { return DIRECT_MAP_BASE; }

u64 vmm_virt_to_phys(void *ptr) {
  u64 virtual_address = (u64)(usize)ptr;
  if (virtual_address >= DIRECT_MAP_BASE && virtual_address < DIRECT_MAP_BASE + DIRECT_MAP_SIZE) {
    return virtual_address - DIRECT_MAP_BASE;
  }
  u64 *pml4 = get_current_pml4();
  if (!pml4) return 0;

  u64 pml4e = pml4[pml4_index(virtual_address)];
  if ((pml4e & VMM_PRESENT) == 0) return 0;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  if ((pdpte & VMM_PRESENT) == 0) return 0;
  if (pdpte & HUGE_PAGE_FLAG) {
    return (pdpte & PAGE_ENTRY_ADDRESS_MASK) + (virtual_address & 0x3FFFFFFF);
  }

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  if ((pde & VMM_PRESENT) == 0) return 0;
  if (pde & HUGE_PAGE_FLAG) {
    return (pde & PAGE_ENTRY_ADDRESS_MASK) + (virtual_address & 0x1FFFFF);
  }

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(virtual_address)];
  if ((pte & VMM_PRESENT) == 0) return 0;

  return (pte & PAGE_ENTRY_ADDRESS_MASK) + (virtual_address & 0xFFF);
}

// Mark a page as lazy (will allocate on first access)
void vmm_set_lazy(u64 virtual_address) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0)
    return;

  u64 *pml4 = get_current_pml4();
  u64 *pdpt = ensure_child_table(pml4, pml4_index(virtual_address));
  pml4[pml4_index(virtual_address)] |= VMM_USER;
  u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual_address));
  pdpt[pdpt_index(virtual_address)] |= VMM_USER;

  /* The low 4 GiB is identity-mapped with 2 MiB SUPERVISOR huge pages in every
   * address space (cloned per-space). The userspace load base 0x2000000 lives
   * inside that region, so a lazy mapping there must SPLIT the huge page into a
   * 4 KiB page table first — exactly as vmm_map_page_in_table does for the eager
   * path. Without this, ensure_child_table below would treat the huge page's
   * physical base as a page-table pointer and scribble VMM_LAZY into arbitrary
   * physical memory (the deterministic clang-entry corruption behind the
   * demand-paged-loader SIGILL). */
  u64 *pt;
  if ((pd[pd_index(virtual_address)] & HUGE_PAGE_FLAG) != 0) {
    pt = split_huge_page(pd, pd_index(virtual_address));
  } else {
    pt = ensure_child_table(pd, pd_index(virtual_address));
  }
  pd[pd_index(virtual_address)] |= VMM_USER;

  // Set a non-present entry with LAZY flag so we know it's a lazy page
  pt[pt_index(virtual_address)] = VMM_LAZY;
  invalidate_page(virtual_address);
}

// Handle page faults for demand paging and swap
/* Walk the CURRENT address space to the leaf PT entry for va, following only
 * already-present, non-huge intermediate tables (no allocation — safe to call
 * under vmm_lock with IRQs off). Returns a pointer to the live pt[idx] slot, or
 * NULL if any level is absent/huge (i.e. there is no leaf PTE to fault on).
 * Caller must hold vmm_lock so the tables can't be torn down mid-walk. */
static u64 *pf_leaf_pte_ptr(u64 va) {
  u64 *pml4 = get_current_pml4();
  u64 pml4e = pml4[pml4_index(va)];
  if ((pml4e & VMM_PRESENT) == 0) return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(va)];
  if ((pdpte & VMM_PRESENT) == 0 || (pdpte & HUGE_PAGE_FLAG)) return 0;
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(va)];
  if ((pde & VMM_PRESENT) == 0 || (pde & HUGE_PAGE_FLAG)) return 0;
  u64 *pt = table_from_entry(pde);
  return &pt[pt_index(va)];
}

/* SMP page-fault handler (M28 #7 — make the fault path self-locking so the
 * exception handler can drop the BKL). The page-table mutators (vmm_map_page,
 * vmm_unmap_page, paging_clone_address_space's CoW write-back) all serialise on
 * vmm_lock; the fault handler is the one mutator that historically did not, so
 * a CLONE_VM sibling faulting on one CPU could RMW the same leaf PTE that fork
 * was CoW-marking on another. vmm_lock is IRQs-off and non-recursive and the
 * file/swap paths block (blk_*_cached → scheduler_yield), so we cannot simply
 * hold it across the handler. Instead use the classic prepare-then-commit
 * shape: allocate the frame and do the blocking I/O OUTSIDE the lock, then take
 * vmm_lock, RE-READ the live leaf PTE, and only install if the precondition
 * (LAZY / SWAPPED / COW with the same old frame) still holds — otherwise
 * another CPU already serviced this address, so we discard our spare frame and
 * report success. */
int vmm_handle_page_fault(u64 fault_addr, u64 error_code) {

  u64 page_aligned = fault_addr & ~(PAGE_SIZE - 1);

  if (!is_canonical(fault_addr)) {
    panic("Non-canonical address fault!");
  }

  extern void eviction_evict_page(void);
  extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
  extern void eviction_unregister_page(u64 frame);

  /* A non-present leaf can be an explicit lazy/swap entry. Do not let the
   * heap growth fast path overwrite that metadata with a zero page. */
  int has_deferred_leaf = 0;
  u64 deferred_flags;
  vmm_read_acquire(&deferred_flags);
  u64 *deferred_leaf = pf_leaf_pte_ptr(page_aligned);
  if (deferred_leaf && (*deferred_leaf & (VMM_LAZY | VMM_SWAPPED)))
    has_deferred_leaf = 1;
  vmm_read_release(deferred_flags);

  // Lazy Allocation for User Heap/Mmap region (anonymous, no leaf PTE yet).
  if (!(error_code & PF_PRESENT) && !has_deferred_leaf &&
      fault_addr >= 0x40000000 &&
      fault_addr < 0x00007FFFFFFFFFFF) {
    /* M88: enforce PROT_NONE. A pure reservation (mmap PROT_NONE) records a VMA
     * but installs no leaf PTE, so without this a wild user access would fall
     * into the zero-fill below and silently succeed. If the faulting address
     * lands in a no-access VMA, refuse to service it — the caller delivers
     * SIGSEGV. The VMA list is sorted by start, so the walk early-exits past the
     * address; this runs only on the anonymous not-present path (PROT_NONE
     * regions never have present/lazy leaves), not on heap-growth faults that
     * have no covering VMA. mprotect splits the VMA and updates ->prot, so a
     * region later made accessible (e.g. V8 committing part of a reservation)
     * has prot != PROT_NONE here and falls through to the normal zero-fill. */
    if ((error_code & PF_USER) && current_task) {
      for (struct vm_area *v = current_task->vma_list;
           v && v->start <= page_aligned; v = v->next) {
        if (page_aligned < v->end) {
          if (v->prot == PROT_NONE)
            return -1; /* no access -> SIGSEGV */
          break;       /* covering VMA grants access; service normally */
        }
      }
    }
    /* Zero-page dedup: a fresh anonymous heap page has no content, so point
     * the mapping at the single shared read-only zero page instead of
     * allocating a frame. The first store faults into the COW path and
     * materialises a private frame (served from the pre-zeroed pool — no
     * memset). Capacity win: brk/mmap regions stay unbacked until written. */
    u64 zero_pg = pmm_zero_page();
    if (zero_pg) {
      u64 cflags;
      vmm_write_acquire(&cflags);
      u64 *slot = pf_leaf_pte_ptr(page_aligned);
      if (slot && (*slot & VMM_PRESENT)) {
        vmm_write_release(cflags);
        return 0; /* already serviced concurrently */
      }
      vmm_map_page_locked(page_aligned, zero_pg,
                          VMM_PRESENT | VMM_COW | VMM_USER);
      vmm_write_release(cflags);
      /* Not registered in the eviction ring: one shared frame, read-only, and
       * swap must never evict a frame mapped in many address spaces. */
      return 0;
    }
    // Prepare a zeroed frame OUTSIDE the lock (alloc may run reclaim/swap).
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      eviction_evict_page();
      frame = pmm_alloc_frame();
      if (!frame) {
        /* True OOM while growing a userspace mapping. Do NOT panic the whole
         * kernel for one greedy process — return failure so the exception
         * handler kills the faulting task with SIGSEGV (Linux-style OOM: the
         * offending process dies, the system survives). Matches the VMM_LAZY
         * path below. Kernel-internal OOM (page tables, klarge, heap growth)
         * still panics — there is no faulting userspace task to blame. */
        console_write("pf: OOM growing userspace mapping for pid ");
        console_write_dec(current_task ? current_task->id : 0);
        console_write(" — killing task\n");
        return -1;
      }
    }
    memset((void *)((u64)frame + DIRECT_MAP_BASE), 0, PAGE_SIZE);

    // Commit under vmm_lock. Re-check that the leaf is still absent — another
    // CPU faulting the same anonymous address may have installed it while we
    // were allocating; if so, discard our spare and let the retry succeed.
    u64 cflags;
    vmm_write_acquire(&cflags);
    u64 *slot = pf_leaf_pte_ptr(page_aligned);
    if (slot && (*slot & VMM_PRESENT)) {
      vmm_write_release(cflags);
      pmm_free_frame(frame);
      return 0; // already serviced concurrently
    }
    vmm_map_page_locked(page_aligned, frame,
                        VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    vmm_write_release(cflags);
    eviction_register_page(current_task, page_aligned, frame);
    return 0;
  }

  /* Classify against the live leaf PTE. Read it under the lock so we get a
   * coherent snapshot, then drop the lock for any blocking work and re-validate
   * on commit. */
  u64 rflags;
  vmm_read_acquire(&rflags);
  u64 *leaf0 = pf_leaf_pte_ptr(page_aligned);
  if (!leaf0) {
    vmm_read_release(rflags);
    return -1;
  }
  u64 pte = *leaf0;
  vmm_read_release(rflags);

  // Case 1: Lazy page (VMM_LAZY leaf, not present) — anonymous or file-backed.
  if (!(pte & VMM_PRESENT) && (pte & VMM_LAZY)) {
    /* Zero-page dedup: classify the faulting address up front. If it is
     * anonymous (no covering file-backed VMA), map the single shared zero
     * page read-only (+COW when the mapping is writable) instead of allocating
     * a frame — the first store materialises a private page via the COW path.
     * File-backed faults skip this and run the normal allocate+fill below. */
    int anon_page = 1;
    struct vm_area *va = current_task->vma_list;
    while (va) {
      if (page_aligned >= va->start && page_aligned < va->end) {
        if (va->node && va->node->inode) {
          struct vfs_inode *in = va->node->inode;
          if (in->type == VFS_FILE || in->read_cb || in->data)
            anon_page = 0;
        }
        break;
      }
      va = va->next;
    }
    if (anon_page && pmm_zero_page()) {
      u64 cflags;
      vmm_write_acquire(&cflags);
      u64 *slot = pf_leaf_pte_ptr(page_aligned);
      if (!slot || (*slot & VMM_PRESENT) || !(*slot & VMM_LAZY)) {
        /* Another CPU serviced it (or it was torn down). */
        vmm_write_release(cflags);
        return (slot && (*slot & VMM_PRESENT)) ? 0 : -1;
      }
      u64 zflags = VMM_PRESENT;
      if (*slot & VMM_WRITABLE) zflags |= VMM_COW; /* first store COWs */
      if (*slot & VMM_USER) zflags |= VMM_USER;
      if (*slot & VMM_NO_EXECUTE) zflags |= VMM_NO_EXECUTE;
      *slot = pmm_zero_page() | zflags;
      invalidate_page(page_aligned);
      vmm_write_release(cflags);
      /* Shared frame: never registered in the eviction ring (swap must not
       * evict a page mapped read-only in many address spaces). */
      return 0;
    }
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      eviction_evict_page();
      frame = pmm_alloc_frame();
      if (!frame) {
        console_write("pf: OOM during lazy allocation, swap failed\n");
        return -1;
      }
    }
    void *new_frame_virt = (void *)((u64)frame + DIRECT_MAP_BASE);
    memset(new_frame_virt, 0, PAGE_SIZE);

    // File-backed fill happens OUTSIDE vmm_lock (read_cb / blk_*_cached block).
    int shared_cache_frame = 0;
    int vma_shared = 0;
    struct vm_area *vma = current_task->vma_list;
    while (vma) {
      if (page_aligned >= vma->start && page_aligned < vma->end) {
        /* MAP_SHARED file pages must stay shared across fork: the page-cache
         * frame is the single backing store for every mapper (e.g. an M48
         * memfd shared between a client and displayd). Without VMM_SHARED the
         * fork CoW path (clone_table) would copy the page on the next write,
         * silently de-sharing the mapping. */
        vma_shared = (vma->flags & MAP_SHARED) != 0;
        if (vma->node && vma->node->inode) {
          u64 file_offset = vma->offset + (page_aligned - vma->start);
          u64 file_page = file_offset & ~(PAGE_SIZE - 1);

          if (vma->node->inode->type == VFS_FILE) {
            /* M72: a writable MAP_SHARED file page is potentially-dirty the
             * moment it is mapped — stores through the PTE won't fault again, so
             * we cannot observe the write later. Mark the page-cache entry dirty
             * now so reactive reclaim writes it back instead of dropping it as
             * "clean", which used to lose mmap stores that raced ahead of msync.
             * Read-only or MAP_PRIVATE mappings are untouched. */
            int mark_dirty = vma_shared && (vma->prot & PROT_WRITE);
            struct page_cache_entry *page = page_cache_get_page(vma->node->inode, file_page);
            if (page) {
              pmm_free_frame(frame); // drop the freshly allocated frame
              frame = page->frame;
              pmm_ref_frame(frame);  // VMA references it
              if (mark_dirty)
                page_cache_mark_dirty(page);
              page_cache_put_page(page);
              shared_cache_frame = 1;
            } else if (vma->node->inode->read_cb) {
              isize res = vma->node->inode->read_cb(vma->node, file_page, (char *)new_frame_virt, PAGE_SIZE, 0);
              if (res >= 0) {
                if (page_cache_add_page(vma->node->inode, file_page, frame) == 0) {
                  pmm_ref_frame(frame); // cache ref + VMA ref
                  /* The frame now belongs to the page cache as well, so it is
                   * shared exactly like the hit path above — every later mapper
                   * of this file page, and every read()/pread(), is served from
                   * it. Say so, or the COW downgrade below does not fire and a
                   * writable MAP_PRIVATE mapping gets the cached frame mapped
                   * WRITABLE: the process's private stores then land straight
                   * in the page cache, silently rewriting the file's contents
                   * for everyone else.
                   *
                   * This is what corrupted libpam.so.2: ELF pads its RX segment
                   * out to the same file page its RW segment starts on, so
                   * ld.so's relocation stores through the writable mapping
                   * overwrote the cached copy of the page that also holds the
                   * library's `.plt` tail. Later mappers executed the resulting
                   * garbage and died on a #UD, and even a plain read() of the
                   * file returned the runtime pointers instead of its code. */
                  shared_cache_frame = 1;
                  if (mark_dirty) {
                    struct page_cache_entry *pe =
                        page_cache_get_page(vma->node->inode, file_page);
                    if (pe) {
                      page_cache_mark_dirty(pe);
                      page_cache_put_page(pe);
                    }
                  }
                }
              } else {
                pmm_free_frame(frame);
                return -1;
              }
            } else if (vma->node->inode->data) {
              /* Initramfs files are resident in inode->data and deliberately
               * have no read callback. Populate mmap pages from that backing
               * store instead of leaving the lazy frame zero-filled. */
              if (file_page < vma->node->inode->size) {
                usize copy_size = vma->node->inode->size - file_page;
                if (copy_size > PAGE_SIZE)
                  copy_size = PAGE_SIZE;
                memcpy(new_frame_virt,
                       (const char *)vma->node->inode->data + file_page,
                       copy_size);
              }
            }
          } else if (vma->node->inode->read_cb) {
            isize res = vma->node->inode->read_cb(vma->node, file_offset, (char *)new_frame_virt, PAGE_SIZE, 0);
            if (res < 0) {
              pmm_free_frame(frame);
              return -1;
            }
          }
        }
        break;
      }
      vma = vma->next;
    }

    // Commit: re-read the leaf; only install if it is still the same LAZY PTE.
    u64 cflags;
    vmm_write_acquire(&cflags);
    u64 *slot = pf_leaf_pte_ptr(page_aligned);
    if (!slot || (*slot & VMM_PRESENT) || !(*slot & VMM_LAZY)) {
      // Another CPU serviced it (or it was torn down) — discard our frame.
      vmm_write_release(cflags);
      if (shared_cache_frame) pmm_unref_frame(frame); else pmm_free_frame(frame);
      return (slot && (*slot & VMM_PRESENT)) ? 0 : -1;
    }
    /* Honour the protection saved in the lazy PTE (paging_mprotect_page stored
     * VMM_WRITABLE/USER at mmap/loader time) instead of forcing writable. A
     * read-only file-backed mapping — e.g. a demand-paged executable's RO text,
     * which shares one refcounted page-cache frame across every mapper — must
     * fault in read-only so a stray write can't corrupt the shared cache page. */
    u64 flags = VMM_PRESENT;
    if (*slot & VMM_WRITABLE) flags |= VMM_WRITABLE;
    if (*slot & VMM_USER) flags |= VMM_USER;
    if (vma_shared) flags |= VMM_SHARED;
    /* A writable MAP_PRIVATE file page must NOT map the shared page-cache
     * frame writable: the first store (e.g. ld.so applying relocations to a
     * library's data segment) would be written INTO the cache and served to
     * every later mapper of the file — the next process's ld.so then reads
     * pre-relocated garbage (observed: libOSMesa's DYNAMIC page carrying
     * another process's relocated pointers → decode_dyn found no DT_HASH →
     * find_sym crash). Map it read-only + COW so the first write copies the
     * page out of the cache, exactly like a forked private page. */
    if (shared_cache_frame && !vma_shared && (flags & VMM_WRITABLE)) {
      flags &= ~VMM_WRITABLE;
      flags |= VMM_COW;
    }
    *slot = frame | flags;
    invalidate_page(page_aligned);
    vmm_write_release(cflags);
    /* Shared page-cache frames are owned by the cache and mapped in several
     * address spaces — leave them out of the per-task swap set (same as SysV
     * shm, which never registers either). */
    if (!vma_shared)
      eviction_register_page(current_task, page_aligned, frame);
    return 0;
  }

  // Case 2: Swapped page (VMM_SWAPPED leaf, not present). The swap slot index is
  // encoded in the PTE's address field — read it directly, no reverse-map scan.
  if (!(pte & VMM_PRESENT) && (pte & VMM_SWAPPED)) {
    // swap_in allocates a frame and does blocking disk I/O — outside the lock.
    u64 new_frame = 0;
    u32 swslot = (u32)((pte & PAGE_ENTRY_ADDRESS_MASK) >> 12);
    if (swap_in(swslot, &new_frame) < 0) {
      console_write("pf: swap in failed for 0x");
      console_write_hex64(page_aligned);
      console_write("\n");
      return -1;
    }

    u64 cflags;
    vmm_write_acquire(&cflags);
    u64 *slot = pf_leaf_pte_ptr(page_aligned);
    if (!slot || (*slot & VMM_PRESENT) || !(*slot & VMM_SWAPPED)) {
      // Raced with another fault/unmap — our frame is now orphaned.
      vmm_write_release(cflags);
      pmm_free_frame(new_frame);
      return (slot && (*slot & VMM_PRESENT)) ? 0 : -1;
    }
    u64 flags = VMM_PRESENT | VMM_WRITABLE;
    if (*slot & VMM_USER) flags |= VMM_USER;
    if (*slot & VMM_NO_EXECUTE) flags |= VMM_NO_EXECUTE;
    *slot = new_frame | flags;
    invalidate_page(page_aligned);
    vmm_write_release(cflags);
    eviction_register_page(current_task, page_aligned, new_frame);
    return 0;
  }

  // Case 3: Copy-on-Write (write to a cloned private page).
  if ((error_code & PF_WRITE) && (pte & VMM_PRESENT) && (pte & VMM_COW)) {
    // Pre-allocate a copy frame outside the lock (we may not need it if we are
    // the last sharer, but allocating speculatively keeps the commit section
    // non-blocking). pmm_alloc_frame fast path doesn't block.
    u64 new_frame = pmm_alloc_frame();
    if (!new_frame) {
      eviction_evict_page();
      new_frame = pmm_alloc_frame();
      if (!new_frame) return -1;
    }

    u64 cflags;
    vmm_write_acquire(&cflags);
    u64 *slot = pf_leaf_pte_ptr(page_aligned);
    if (!slot || !(*slot & VMM_PRESENT) || !(*slot & VMM_COW)) {
      // Another CPU already resolved the COW for this page.
      vmm_write_release(cflags);
      pmm_free_frame(new_frame);
      return 0;
    }
    u64 cur = *slot;
    u64 old_frame = cur & PAGE_ENTRY_ADDRESS_MASK;
    u64 new_flags = (cur & ~PAGE_ENTRY_ADDRESS_MASK);
    new_flags &= ~VMM_COW;
    new_flags |= VMM_PRESENT | VMM_WRITABLE;

    if (old_frame == pmm_zero_page()) {
      /* First write to the shared zero page (zero-page dedup): the spare frame
       * is already zero-filled, so there is nothing to copy, and the zero page
       * must never be unreferenced or eviction-unregistered (it is shared and
       * has a permanent reservation). */
      *slot = new_frame | new_flags;
      invalidate_page(page_aligned);
      vmm_write_release(cflags);
      eviction_register_page(current_task, page_aligned, new_frame);
      return 0;
    }

    if (pmm_get_refcount(old_frame) == 1) {
      // Sole owner now — just flip to writable in place, no copy needed.
      *slot = old_frame | new_flags;
      invalidate_page(page_aligned);
      vmm_write_release(cflags);
      pmm_free_frame(new_frame); // didn't need the spare
      return 0;
    }

    // Shared: copy into the spare frame and point this mapping at it. The copy
    // touches only the two physical frames via the direct map (not the fault
    // va), so it is safe under the lock.
    memcpy((void *)(usize)(new_frame + DIRECT_MAP_BASE),
           (void *)(usize)(old_frame + DIRECT_MAP_BASE), PAGE_SIZE);
    *slot = new_frame | new_flags;
    pmm_unref_frame(old_frame);
    invalidate_page(page_aligned);
    vmm_write_release(cflags);

    eviction_unregister_page(old_frame);
    eviction_register_page(current_task, page_aligned, new_frame);
    return 0;
  }

  return -1; // Unhandled
}

u64 paging_create_address_space(void) {
  u64 *pml4 = alloc_page_table();
  u64 pml4_phys = table_to_phys(pml4);

  u64 _vmflags;
  vmm_read_acquire(&_vmflags);

  // Clone kernel-half entries (256-511)
  for (usize i = 256; i < 512; i++) {
    pml4[i] = kernel_pml4_virt[i];
  }

  // Instead of sharing PML4 entry 0 directly, we allocate a private PDPT for PML4 entry 0
  // and clone the kernel's identity mapping (0-4 GB) into it.
  u64 *kernel_pml4 = kernel_pml4_virt;
  if (kernel_pml4[0] & VMM_PRESENT) {
    u64 *kernel_pdpt = table_from_entry(kernel_pml4[0]);
    u64 *dst_pdpt = alloc_page_table();
    pml4[0] = table_to_phys(dst_pdpt) | (kernel_pml4[0] & ~PAGE_ENTRY_ADDRESS_MASK);

    // Clone the first 4 entries of the kernel's PDPT (which cover 0-4 GB)
    for (usize j = 0; j < 4; j++) {
      if (kernel_pdpt[j] & VMM_PRESENT) {
        if (kernel_pdpt[j] & HUGE_PAGE_FLAG) {
          dst_pdpt[j] = kernel_pdpt[j];
        } else {
          // Allocate a private PD for this gigabyte
          u64 *kernel_pd = table_from_entry(kernel_pdpt[j]);
          u64 *dst_pd = alloc_page_table();
          dst_pdpt[j] = table_to_phys(dst_pd) | (kernel_pdpt[j] & ~PAGE_ENTRY_ADDRESS_MASK);

          // Copy all 512 entries of this PD (these are the 2MB huge pages)
          for (usize k = 0; k < 512; k++) {
            dst_pd[k] = kernel_pd[k];
          }
        }
      }
    }
  }

  vmm_read_release(_vmflags);
  return pml4_phys;
}

/* Drain any in-flight cross-CPU TLB shootdown (defined in arch/x86/tlb.c).
 * clone_table / free_table / swap_in_recursive walk the whole user page-table
 * tree doing real work — not spinning — while the caller holds an IRQs-off
 * section (paging_clone_address_space takes vmm_write_lock via its _irqsave
 * variant; the exit reaper frees with interrupts disabled). With IRQs masked
 * and no spin loop, such a CPU never ACKs a shootdown IPI, so an initiator on
 * another core hits `tlb: shootdown stalled, pending=1` and panics — exactly
 * the smp>=2 in-guest-build failure. A poll once per visited table keeps the
 * ACK latency far under the runaway guard; it is a single atomic load on the
 * common (nothing-pending) path, so uniprocessor and uncontended SMP pay
 * nothing. */
void tlb_shootdown_poll(void);

static void clone_table(u64 *src_table, u64 *dst_table, int level) {
  tlb_shootdown_poll();
  for (usize i = 0; i < 512; i++) {
    if (!(src_table[i] & VMM_PRESENT)) {
      dst_table[i] = src_table[i]; // Copy lazy/swapped entries as is
      continue;
    }

    u64 entry = src_table[i];

    // If it's a huge page (PS bit set), it's a leaf entry (1GB at level 1, or 2MB at level 2)
    if (entry & HUGE_PAGE_FLAG) {
      dst_table[i] = entry;
      continue;
    }

    if (level < 3) {
      // PML4, PDPT, or PD -> recurse
      u64 *src_child = table_from_entry(entry);
      u64 *dst_child = alloc_page_table();
      dst_table[i] = table_to_phys(dst_child) | (entry & ~PAGE_ENTRY_ADDRESS_MASK);
      clone_table(src_child, dst_child, level + 1);
    } else {
      // PT -> copy frame and handle CoW
      u64 frame = entry & PAGE_ENTRY_ADDRESS_MASK;

      if (entry & VMM_USER) {
        if ((entry & VMM_WRITABLE) && !(entry & VMM_SHARED)) {
          // Mark both as Read-Only for CoW
          entry &= ~VMM_WRITABLE;
          entry |= VMM_COW;
          src_table[i] = entry;
        }

        dst_table[i] = entry;
        if (frame && frame != pmm_zero_page()) {
          /* The shared zero page keeps its permanent reservation refcount; a
           * fork of an untouched (still shared) anonymous page adds no
           * reference, and pmm_free_frame ignores it on teardown anyway. */
          pmm_ref_frame(frame);
        }
      } else {
        dst_table[i] = entry;
      }
    }
  }
}

u64 paging_clone_address_space(u64 src_pml4_phys) {
  u64 real_src_phys = src_pml4_phys ? src_pml4_phys : kernel_pml4_phys;
  u64 *src_pml4 = (u64 *)(usize)(real_src_phys + DIRECT_MAP_BASE);
  u64 *dst_pml4 = alloc_page_table();
  u64 dst_pml4_phys = table_to_phys(dst_pml4);

  /* M28 #7 T4: serialize page-table reads against concurrent vmm_map_page
   * /vmm_unmap_page writes. Without BKL the syscall path lets two CPUs run
   * mm syscalls in parallel; a fork that walks its src pml4 while another
   * core mutates the same pml4 (mmap, page-fault CoW handler, eviction)
   * sees torn flag/address words and produces a corrupt child mapping that
   * faults the moment the child enters ring 3. The clone_table CoW pass
   * also writes back to src PT entries, so we need the write side. */
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);

  // Clone user-half entries (0-255)
  for (usize i = 0; i < 256; i++) {
    if (src_pml4[i] & VMM_PRESENT) {
      u64 *src_pdpt = table_from_entry(src_pml4[i]);
      u64 *dst_pdpt = alloc_page_table();
      dst_pml4[i] = table_to_phys(dst_pdpt) | (src_pml4[i] & ~PAGE_ENTRY_ADDRESS_MASK);
      clone_table(src_pdpt, dst_pdpt, 1);
    }
  }

  // Copy kernel-half entries (256-511)
  for (usize i = 256; i < 512; i++) {
    dst_pml4[i] = kernel_pml4_virt[i];
  }

  if (real_src_phys == read_cr3()) {
    write_cr3(real_src_phys);
  }

  vmm_write_release(_vmflags);

  return dst_pml4_phys;
}


/* Mark vaddr's leaf PTE as swapped, storing the swap SLOT index in the PTE's
 * (now-unused) address field. The page is non-present, so the address bits are
 * free to carry the slot; the #PF handler reads it back to drive swap_in with no
 * reverse-map table. */
void paging_mark_swapped(u64 pml4_phys, u64 vaddr, u64 slot) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT)) return;
  if (pdpte & HUGE_PAGE_FLAG) return;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT)) return;
  if (pde & HUGE_PAGE_FLAG) return;

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(vaddr)];

  if (pte & VMM_PRESENT) {
    u64 flags = pte & ~PAGE_ENTRY_ADDRESS_MASK;
    flags &= ~VMM_PRESENT;
    flags |= VMM_SWAPPED;
    pt[pt_index(vaddr)] = ((slot << 12) & PAGE_ENTRY_ADDRESS_MASK) | flags;
    invalidate_page(vaddr);
  }
}

/* Swap slots are now freed incrementally as their pages are unmapped
 * (unmap_page_from_pml4) and as a teardown backstop in free_table — there is no
 * separate per-exit page-table walk. A standalone full-tree walk on every exit
 * (the first cut of this) regressed the heavy multi-threaded smoke instances;
 * folding the slot-free into the existing unmap/teardown paths is both correct
 * and free. Kept as a no-op so swap_free_all_slots has a stable callee. */
void paging_free_swap_slots(u64 pml4_phys) {
  (void)pml4_phys;
}

int paging_test_and_clear_accessed(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return 0;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT)) return 0;
  if (pdpte & HUGE_PAGE_FLAG) return 0;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT)) return 0;
  if (pde & HUGE_PAGE_FLAG) return 0;

  u64 *pt = table_from_entry(pde);
  if (!(pt[pt_index(vaddr)] & VMM_PRESENT)) return 0;

  /* Atomic clear: the MMU sets the Accessed/Dirty bits in this PTE from other
   * CPUs concurrently. A plain read-modify-write would clobber a hardware A/D
   * update landing between the read and the store, losing it. `lock and` (via
   * __atomic_fetch_and) clears our bit while preserving every other bit the
   * hardware races to set, and returns the prior value so we can test it. */
  u64 old = __atomic_fetch_and((u64 *)&pt[pt_index(vaddr)], ~VMM_ACCESSED,
                               __ATOMIC_SEQ_CST);
  if (old & VMM_ACCESSED) {
    invalidate_page(vaddr);
    return 1;
  }
  return 0;
}

/* M72: test-and-clear the hardware dirty bit of a 4 KiB user page. Used by
 * msync to discover which MAP_SHARED pages userspace actually wrote (the CPU
 * sets PTE.D on the store) so only those are written back to the file. Returns
 * 1 if the page was dirty (and clears the bit), 0 otherwise. */
int paging_test_and_clear_dirty(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return 0;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT)) return 0;
  if (pdpte & HUGE_PAGE_FLAG) return 0;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT)) return 0;
  if (pde & HUGE_PAGE_FLAG) return 0;

  u64 *pt = table_from_entry(pde);
  if (!(pt[pt_index(vaddr)] & VMM_PRESENT)) return 0;

  /* Atomic clear (see paging_test_and_clear_accessed): `lock and` so a
   * concurrent hardware Accessed/Dirty set on another CPU is not lost. */
  u64 old = __atomic_fetch_and((u64 *)&pt[pt_index(vaddr)], ~VMM_DIRTY,
                               __ATOMIC_SEQ_CST);
  if (old & VMM_DIRTY) {
    invalidate_page(vaddr);
    return 1;
  }
  return 0;
}

/* M72: physical frame backing a user vaddr in the given address space (0 if not
 * present). Used by msync to write an mmap'd page's frame straight to its file. */
u64 paging_user_frame(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT) || (pdpte & HUGE_PAGE_FLAG)) return 0;
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT) || (pde & HUGE_PAGE_FLAG)) return 0;
  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(vaddr)];
  if (!(pte & VMM_PRESENT)) return 0;
  return pte & 0x000FFFFFFFFFF000ULL;
}

/* Physical address backing `vaddr`, resolving 1 GiB and 2 MiB mappings as well
 * as 4 KiB ones. paging_user_frame above answers only for a 4 KiB leaf and
 * reports "not mapped" for a huge page — fine for callers that want to install
 * a page, wrong for anyone reading memory: b1nix maps large runs with huge
 * pages, so a reader built on the 4 KiB-only walk (ptrace PEEK,
 * /proc/<pid>/mem, process_vm_readv) sees whole regions of a live process as
 * absent. Returns 0 when nothing is mapped. */
u64 paging_user_phys(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE)
                                       : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT))
    return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT))
    return 0;
  if (pdpte & HUGE_PAGE_FLAG) /* 1 GiB page */
    return (pdpte & 0x000FFFFFC0000000ULL) + (vaddr & 0x3FFFFFFFULL);
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT))
    return 0;
  if (pde & HUGE_PAGE_FLAG) /* 2 MiB page */
    return (pde & 0x000FFFFFFFE00000ULL) + (vaddr & 0x1FFFFFULL);
  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(vaddr)];
  if (!(pte & VMM_PRESENT))
    return 0;
  return (pte & 0x000FFFFFFFFFF000ULL) + (vaddr & 0xFFFULL);
}

/* Raw 4 KiB leaf PTE backing `virtual_address`, or 0 when the address is not
 * mapped by a 4 KiB leaf (unmapped, or covered by a 1 GiB / 2 MiB page). The
 * memory-type bits (PAT/PCD/PWT) live in this entry, so M98's WC self-test
 * reads it back rather than trusting the flags it passed to vmm_map_page. */
u64 paging_leaf_pte(u64 virtual_address) {
  u64 *pml4 = get_current_pml4();
  u64 pml4e = pml4[pml4_index(virtual_address)];
  if (!(pml4e & VMM_PRESENT))
    return 0;
  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  if (!(pdpte & VMM_PRESENT) || (pdpte & HUGE_PAGE_FLAG))
    return 0;
  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  if (!(pde & VMM_PRESENT) || (pde & HUGE_PAGE_FLAG))
    return 0;
  u64 *pt = table_from_entry(pde);
  return pt[pt_index(virtual_address)];
}

void paging_dump_entries(u64 virtual_address) {
  u64 *pml4 = get_current_pml4();
  u64 pml4e = pml4[pml4_index(virtual_address)];
  console_write("PML4E for "); console_write_hex64(virtual_address); console_write(": 0x"); console_write_hex64(pml4e); console_write("\n");
  if (!(pml4e & VMM_PRESENT)) return;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(virtual_address)];
  console_write("  PDPTE: 0x"); console_write_hex64(pdpte); console_write("\n");
  if (!(pdpte & VMM_PRESENT)) return;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(virtual_address)];
  console_write("  PDE:   0x"); console_write_hex64(pde); console_write("\n");
  if (!(pde & VMM_PRESENT)) return;

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(virtual_address)];
  console_write("  PTE:   0x"); console_write_hex64(pte); console_write("\n");
}

static u64 freed_tables_count = 0;

static void free_table(u64 *table, int level) {
  tlb_shootdown_poll(); /* see clone_table: drain shootdowns from this IRQs-off walk */
  if (level >= 3) {
    /* PT level: free the leaf user data frames. VMA-backed pages were already
     * unmapped (PTE cleared) by user_address_space_cleanup, so they are skipped
     * here; what remains present is memory NOT covered by a VMA — chiefly the
     * brk heap (cc1 grows it by tens of MB per compile). Without freeing it here
     * those frames leak permanently across process teardown (OOM after many
     * spawns). pmm_free_frame is refcount-aware, so shared/COW frames are safe. */
    for (usize i = 0; i < 512; i++) {
      u64 entry = table[i];
      if ((entry & VMM_PRESENT) && (entry & VMM_USER)) {
        u64 frame = entry & PAGE_ENTRY_ADDRESS_MASK;
        if (frame) {
          pmm_free_frame(frame);
          freed_tables_count++;
        }
      } else if (!(entry & VMM_PRESENT) && (entry & VMM_SWAPPED)) {
        /* A swapped page that was never VMA-unmapped (e.g. brk frames past the
         * heap VMA): release its swap slot during the exclusive teardown. */
        extern void swap_free_slot_index(u32 slot);
        swap_free_slot_index((u32)((entry & PAGE_ENTRY_ADDRESS_MASK) >> 12));
      }
    }
    return;
  }
  for (usize i = 0; i < 512; i++) {
    u64 entry = table[i];
    if ((entry & VMM_PRESENT) && !(entry & HUGE_PAGE_FLAG)) {
      u64 *child = table_from_entry(entry);
      u64 child_phys = entry & PAGE_ENTRY_ADDRESS_MASK;
      free_table(child, level + 1);
      pmm_free_frame(child_phys);
      freed_tables_count++;
    }
  }
}

void paging_free_address_space(u64 pml4_phys) {
  if (pml4_phys == 0 || pml4_phys == kernel_pml4_phys) {
    return;
  }

  freed_tables_count = 0;
  u64 *pml4 = (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE);

  // Free user-half entries (0-255)
  for (usize i = 0; i < 256; i++) {
    u64 entry = pml4[i];
    if ((entry & VMM_PRESENT) && !(entry & HUGE_PAGE_FLAG)) {
      u64 *pdpt = table_from_entry(entry);
      u64 pdpt_phys = entry & PAGE_ENTRY_ADDRESS_MASK;
      free_table(pdpt, 1);
      pmm_free_frame(pdpt_phys);
      freed_tables_count++;
    }
  }

  // Free the PML4 itself
  pmm_free_frame(pml4_phys);
  freed_tables_count++;
}

static void swap_in_recursive(u64 *table, int level, u64 base_addr, u64 pml4_phys) {
  tlb_shootdown_poll(); /* see clone_table: drain shootdowns from this IRQs-off walk */
  if (level >= 3) {
    for (usize i = 0; i < 512; i++) {
      u64 entry = table[i];
      if (!(entry & VMM_PRESENT) && (entry & VMM_SWAPPED)) {
        u64 vaddr = base_addr + (i * PAGE_SIZE);
        u64 new_frame = 0;
        extern int swap_in(u32 slot, u64 *out_physical_frame);
        u32 swslot = (u32)((entry & PAGE_ENTRY_ADDRESS_MASK) >> 12);
        if (swap_in(swslot, &new_frame) == 0) {
          u64 flags = VMM_PRESENT | VMM_WRITABLE;
          if (entry & VMM_USER) flags |= VMM_USER;
          if (entry & VMM_NO_EXECUTE) flags |= VMM_NO_EXECUTE;
          table[i] = new_frame | flags;
          invalidate_page(vaddr);
          
          extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
          eviction_register_page(current_task, vaddr, new_frame);
        }
      }
    }
    return;
  }

  for (usize i = 0; i < 512; i++) {
    if (level == 0 && i >= 256) break; // Only check user space (0-255)

    u64 entry = table[i];
    if ((entry & VMM_PRESENT) && !(entry & HUGE_PAGE_FLAG)) {
      u64 *child = table_from_entry(entry);
      u64 step = 1ULL << (12 + (3 - level) * 9);
      swap_in_recursive(child, level + 1, base_addr + i * step, pml4_phys);
    }
  }
}

void paging_swap_in_all_swapped(u64 pml4_phys) {
  if (pml4_phys == 0 || pml4_phys == kernel_pml4_phys) {
    return;
  }
  u64 *pml4 = (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE);
  swap_in_recursive(pml4, 0, 0, pml4_phys);
}
