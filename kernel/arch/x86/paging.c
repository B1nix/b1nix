#include <b1nix/console.h>
#include <b1nix/lockdep.h>
#include <b1nix/sched.h>
#include <b1nix/mm.h>
#include <b1nix/rwlock.h>
#include <b1nix/vfs.h>
#include <b1nix/page_cache.h>
#include <b1nix/panic.h>
#include <string.h>

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

#define PAGE_ENTRY_ADDRESS_MASK 0xfffff000UL
#define PAGE_TABLE_INDEX_MASK 0x3ffUL
#define HUGE_PAGE_FLAG (1UL << 7)

#define MMIO_MAP_BASE 0xd0000000UL
#define MMIO_MAP_SIZE (256UL * 1024UL * 1024UL)

static u32 *kernel_pml4_virt; /* Name matches 64-bit source compat (it's the master PD) */
static u32 kernel_pml4_phys;
int direct_map_ready;
static u32 mmio_next = MMIO_MAP_BASE;
static volatile u64 freed_tables_count = 0;

static u32 *get_current_pml4(void) {
  if (current_task && current_task->pml4_phys) {
    return (u32 *)(usize)(current_task->pml4_phys + DIRECT_MAP_BASE);
  }
  return kernel_pml4_virt;
}

static inline void invalidate_page(u32 virtual_address) {
  __asm__ volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

static usize pd_index(u32 virtual_address) {
  return (virtual_address >> 22) & PAGE_TABLE_INDEX_MASK;
}

static usize pt_index(u32 virtual_address) {
  return (virtual_address >> 12) & PAGE_TABLE_INDEX_MASK;
}

static u32 *table_from_entry(u32 entry) {
  u32 phys = entry & PAGE_ENTRY_ADDRESS_MASK;
  return (u32 *)(usize)(phys + DIRECT_MAP_BASE);
}

static u32 table_to_phys(u32 *table) {
  u32 phys = (u32)(usize)table;
  if (phys >= DIRECT_MAP_BASE)
    phys -= DIRECT_MAP_BASE;
  return phys;
}

static u32 *alloc_page_table(void) {
  u64 frame = pmm_alloc_frame();
  if (frame == 0) {
    panic("vmm: OOM during page table allocation");
  }

  u32 reachable_limit = DIRECT_MAP_MAX;
  if (frame >= reachable_limit) {
    panic("vmm: page table frame beyond reachable map");
  }

  u32 *table = (u32 *)(usize)(frame + DIRECT_MAP_BASE);
  memset(table, 0, PAGE_SIZE);
  return table;
}

static u32 *ensure_child_table(u32 *parent, usize index) {
  if (!parent) {
    panic("ensure_child_table: parent is NULL");
  }
  u32 *result;
  if ((parent[index] & VMM_PRESENT) == 0) {
    u32 *child = alloc_page_table();
    parent[index] = table_to_phys(child) | VMM_PRESENT | VMM_WRITABLE;
    result = child;
  } else {
    result = table_from_entry(parent[index]);
  }
  return result;
}

void vmm_map_page_in_table(u32 *pd, u32 virtual_address, u32 physical_address, u32 flags) {
  u32 *pt = ensure_child_table(pd, pd_index(virtual_address));
  pt[pt_index(virtual_address)] = (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
  invalidate_page(virtual_address);
}

void vmm_init(void) {
  u64 phys_pml4 = pmm_alloc_frame();
  u32 *pd = (u32 *)(usize)phys_pml4;
  memset(pd, 0, PAGE_SIZE);

  /* Map physical memory via 4MB huge pages (PSE) */
  for (u32 physical = 0; physical < DIRECT_MAP_SIZE; physical += 0x400000UL) {
    u32 virtual_high = DIRECT_MAP_BASE + physical;

    /* Higher-half mapping */
    pd[pd_index(virtual_high)] = physical | VMM_PRESENT | VMM_WRITABLE | HUGE_PAGE_FLAG;

    /* Identity mapping (transition) */
    pd[pd_index(physical)] = physical | VMM_PRESENT | VMM_WRITABLE | HUGE_PAGE_FLAG;
  }

  /* Pre-allocate every kernel-half page table above the direct map (kheap,
   * MMIO and the klarge arena, PD indices pd_index(KHEAP_START)..1023). This is
   * essential on 32-bit: paging_create_address_space clones the kernel-half PD
   * entries (512-1023) by value at address-space creation time. If a PD entry
   * is empty then and the kernel heap later grows into that 4 MB region, the new
   * PT/PDE is installed only in whatever address space happened to be current —
   * other (already-created) address spaces never see it, so a kernel pointer
   * into the grown heap faults there (kmalloc #PF with PDE==0). Allocating all
   * the PTs up front means every clone copies PDEs that point at these shared
   * tables, so any later kernel mapping lands in a table visible everywhere.
   * ~1 MB of PTs once at boot; the direct-map region below 0xC0000000 keeps its
   * 4 MB huge-page PDEs and is intentionally skipped. */
  for (usize i = pd_index(KHEAP_START); i < 1024; i++) {
    ensure_child_table(pd, i);
  }

  console_write("vmm: direct map 0x");
  console_write_hex32(DIRECT_MAP_BASE);
  console_write("-0x");
  console_write_hex32(DIRECT_MAP_BASE + DIRECT_MAP_SIZE);
  console_write("\n");

  /* Switch to new page table */
  extern void paging_switch_address_space(u64 pml4_phys);
  paging_switch_address_space(phys_pml4);

  direct_map_ready = 1;
  kernel_pml4_virt = (u32 *)(usize)(phys_pml4 + DIRECT_MAP_BASE);
  kernel_pml4_phys = (u32)phys_pml4;

  extern void pmm_switch_to_direct_map(void);
  pmm_switch_to_direct_map();
}

static void vmm_map_page_locked(u32 virtual_address, u32 physical_address, u32 flags) {
  u32 *pd = get_current_pml4();
  u32 *pt = ensure_child_table(pd, pd_index(virtual_address));
  if ((flags & VMM_USER) != 0) {
    pd[pd_index(virtual_address)] |= VMM_USER;
  }
  pt[pt_index(virtual_address)] = (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
  invalidate_page(virtual_address);
}

void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags) {
  u32 va = (u32)virtual_address;
  u32 pa = (u32)physical_address;
  if ((va & (PAGE_SIZE - 1)) != 0 || (pa & (PAGE_SIZE - 1)) != 0) {
    panic("vmm_map_page requires page-aligned addresses");
  }

  if (kernel_pml4_virt == 0) {
    panic("vmm_map_page called before vmm_init");
  }

  u64 _vmflags;
  vmm_write_acquire(&_vmflags);
  vmm_map_page_locked(va, pa, (u32)flags);
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

  u32 phys_base = (u32)physical_address & ~(PAGE_SIZE - 1);
  u32 phys_offset = (u32)physical_address - phys_base;
  u32 total_size = (u32)size + phys_offset;
  u32 map_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  u32 virt_base = (mmio_next + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (virt_base < MMIO_MAP_BASE || virt_base + map_size > MMIO_MAP_BASE + MMIO_MAP_SIZE) {
    panic("vmm_map_mmio exhausted mmio virtual range");
  }

  for (u32 off = 0; off < map_size; off += PAGE_SIZE) {
    vmm_map_page(virt_base + off, phys_base + off, flags | VMM_PRESENT);
  }

  mmio_next = virt_base + map_size;
  return (void *)(usize)(virt_base + phys_offset);
}

static void unmap_page_from_pml4(u32 *pd, u32 virtual_address) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0) {
    panic("vmm_unmap_page requires page-aligned address");
  }

  u32 pde = pd[pd_index(virtual_address)];
  if ((pde & VMM_PRESENT) == 0 || (pde & HUGE_PAGE_FLAG)) {
    return;
  }

  u32 *pt = table_from_entry(pde);
  u32 pte = pt[pt_index(virtual_address)];
  if (pte & VMM_PRESENT) {
    u32 frame = pte & PAGE_ENTRY_ADDRESS_MASK;
    if (frame) {
      if (pte & VMM_USER) {
        pmm_free_frame(frame);
        extern void eviction_unregister_page(u64 frame);
        eviction_unregister_page(frame);
      }
    }
  }
  pt[pt_index(virtual_address)] = 0;
  invalidate_page(virtual_address);
}

void vmm_unmap_page(u64 virtual_address) {
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);
  unmap_page_from_pml4(get_current_pml4(), (u32)virtual_address);
  vmm_write_release(_vmflags);

  extern void tlb_shootdown_page(u64);
  tlb_shootdown_page(virtual_address);
}

void paging_unmap_page_from_space(u64 pml4_phys, u64 virtual_address) {
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);
  u32 *pd = pml4_phys ? (u32 *)(usize)(pml4_phys + DIRECT_MAP_BASE) : kernel_pml4_virt;
  unmap_page_from_pml4(pd, (u32)virtual_address);
  vmm_write_release(_vmflags);
}

void paging_map_page(u64 virtual_address, u64 physical_address, u64 flags) {
  vmm_map_page(virtual_address, physical_address, flags);
}

void paging_unmap_page(u64 virtual_address) { vmm_unmap_page(virtual_address); }

void paging_mprotect_page(u64 virtual_address, u64 flags) {
  u32 va = (u32)virtual_address;
  if ((va & (PAGE_SIZE - 1)) != 0) {
    panic("paging_mprotect_page requires page-aligned address");
  }

  u32 *pd = get_current_pml4();
  u32 pde = pd[pd_index(va)];
  if ((pde & VMM_PRESENT) == 0 || (pde & HUGE_PAGE_FLAG))
    return;

  u32 *pt = table_from_entry(pde);
  u32 pte = pt[pt_index(va)];

  if (pte & VMM_PRESENT) {
    pt[pt_index(va)] = (pte & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
  } else if (pte & (VMM_LAZY | VMM_SWAPPED)) {
    pt[pt_index(va)] =
        (pte & (PAGE_ENTRY_ADDRESS_MASK | VMM_LAZY | VMM_SWAPPED)) | flags;
  }

  invalidate_page(va);
}

void vmm_remap_page(u64 virtual_address, u64 physical_address, u64 flags) {
  vmm_unmap_page(virtual_address);
  vmm_map_page(virtual_address, physical_address, flags);
}

u64 vmm_direct_map_base(void) { return DIRECT_MAP_BASE; }

u64 vmm_virt_to_phys(void *ptr) {
  u32 virtual_address = (u32)(usize)ptr;
  if (virtual_address >= DIRECT_MAP_BASE && virtual_address < DIRECT_MAP_BASE + DIRECT_MAP_SIZE) {
    return virtual_address - DIRECT_MAP_BASE;
  }
  u32 *pd = get_current_pml4();
  if (!pd) return 0;

  u32 pde = pd[pd_index(virtual_address)];
  if ((pde & VMM_PRESENT) == 0) return 0;
  if (pde & HUGE_PAGE_FLAG) {
    return (pde & PAGE_ENTRY_ADDRESS_MASK) + (virtual_address & 0x3FFFFF);
  }

  u32 *pt = table_from_entry(pde);
  u32 pte = pt[pt_index(virtual_address)];
  if ((pte & VMM_PRESENT) == 0) return 0;

  return (pte & PAGE_ENTRY_ADDRESS_MASK) + (virtual_address & 0xFFF);
}

void vmm_set_lazy(u64 virtual_address) {
  u32 va = (u32)virtual_address;
  if ((va & (PAGE_SIZE - 1)) != 0)
    return;

  u32 *pd = get_current_pml4();
  u32 *pt = ensure_child_table(pd, pd_index(va));
  pd[pd_index(va)] |= VMM_USER;

  pt[pt_index(va)] = VMM_LAZY;
  invalidate_page(va);
}

static u32 *pf_leaf_pte_ptr(u32 va) {
  u32 *pd = get_current_pml4();
  u32 pde = pd[pd_index(va)];
  if ((pde & VMM_PRESENT) == 0 || (pde & HUGE_PAGE_FLAG)) return 0;
  u32 *pt = table_from_entry(pde);
  return &pt[pt_index(va)];
}

int vmm_handle_page_fault(u64 fault_addr, u64 error_code) {
  u32 va = (u32)fault_addr;
  u32 page_aligned = va & ~(PAGE_SIZE - 1);

  extern void eviction_evict_page(void);
  extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
  extern void eviction_unregister_page(u64 frame);

  /* A non-present leaf can be an explicit lazy/swap entry. Do not let the
   * heap/stack growth fast path overwrite that metadata with a zero page. */
  int has_deferred_leaf = 0;
  u64 deferred_flags;
  vmm_read_acquire(&deferred_flags);
  u32 *deferred_leaf = pf_leaf_pte_ptr(page_aligned);
  if (deferred_leaf && (*deferred_leaf & (VMM_LAZY | VMM_SWAPPED)))
    has_deferred_leaf = 1;
  vmm_read_release(deferred_flags);

  // Lazy Allocation for User Heap/Mmap and the downward-growing user stack.
  // The upper bound is USER_STACK_TOP (0xC0000000): the stack lives just below
  // it (USER_STACK_TOP - 8 MB .. USER_STACK_TOP) and is mapped one page at a
  // time, so on-demand faults there must allocate (the old 0x80000000 ceiling
  // stopped at the direct-map base and left every stack-growth fault unhandled,
  // killing any non-trivial ELF32 process). Direct-map pages (0x80000000..) are
  // PRESENT, so the !PF_PRESENT guard already excludes them.
  if (!(error_code & PF_PRESENT) && !has_deferred_leaf &&
      va >= 0x40000000 && va < 0xC0000000) {
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      eviction_evict_page();
      frame = pmm_alloc_frame();
      if (!frame) {
        console_write("pf: OOM growing userspace mapping — killing task\n");
        return -1;
      }
    }
    memset((void *)((u32)frame + DIRECT_MAP_BASE), 0, PAGE_SIZE);

    u64 cflags;
    vmm_write_acquire(&cflags);
    u32 *slot = pf_leaf_pte_ptr(page_aligned);
    if (slot && (*slot & VMM_PRESENT)) {
      vmm_write_release(cflags);
      pmm_free_frame(frame);
      return 0;
    }
    vmm_map_page_locked(page_aligned, (u32)frame, VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    vmm_write_release(cflags);
    eviction_register_page(current_task, page_aligned, frame);
    return 0;
  }

  u64 rflags;
  vmm_read_acquire(&rflags);
  u32 *leaf0 = pf_leaf_pte_ptr(page_aligned);
  if (!leaf0) {
    vmm_read_release(rflags);
    return -1;
  }
  u32 pte = *leaf0;
  vmm_read_release(rflags);

  // Case 1: Lazy page
  if (!(pte & VMM_PRESENT) && (pte & VMM_LAZY)) {
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      eviction_evict_page();
      frame = pmm_alloc_frame();
      if (!frame) return -1;
    }
    void *new_frame_virt = (void *)((u32)frame + DIRECT_MAP_BASE);
    memset(new_frame_virt, 0, PAGE_SIZE);

    int shared_cache_frame = 0;
    int vma_shared = 0;
    struct vm_area *vma = current_task->vma_list;
    while (vma) {
      if (page_aligned >= vma->start && page_aligned < vma->end) {
        /* MAP_SHARED file pages must stay shared across fork (M48 memfd) — see
         * the x86_64 paging.c note. Without VMM_SHARED the fork CoW path copies
         * the page on the next write and silently de-shares the mapping. */
        vma_shared = (vma->flags & MAP_SHARED) != 0;
        if (vma->node && vma->node->inode) {
          u64 file_offset = vma->offset + (page_aligned - vma->start);
          u64 file_page = file_offset & ~(PAGE_SIZE - 1);

          if (vma->node->inode->type == VFS_FILE) {
            struct page_cache_entry *page = page_cache_get_page(vma->node->inode, file_page);
            if (page) {
              pmm_free_frame(frame);
              frame = page->frame;
              pmm_ref_frame(frame);
              page_cache_put_page(page);
              shared_cache_frame = 1;
            } else if (vma->node->inode->read_cb) {
              isize res = vma->node->inode->read_cb(vma->node, file_page, (char *)new_frame_virt, PAGE_SIZE, 0);
              if (res >= 0) {
                if (page_cache_add_page(vma->node->inode, file_page, frame) == 0) {
                  pmm_ref_frame(frame);
                }
              } else {
                pmm_free_frame(frame);
                return -1;
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

    u64 cflags;
    vmm_write_acquire(&cflags);
    u32 *slot = pf_leaf_pte_ptr(page_aligned);
    if (!slot || (*slot & VMM_PRESENT) || !(*slot & VMM_LAZY)) {
      vmm_write_release(cflags);
      if (shared_cache_frame) pmm_unref_frame(frame); else pmm_free_frame(frame);
      return (slot && (*slot & VMM_PRESENT)) ? 0 : -1;
    }
    u32 flags = VMM_PRESENT | VMM_WRITABLE;
    if (*slot & VMM_USER) flags |= VMM_USER;
    if (vma_shared) flags |= VMM_SHARED;
    *slot = (u32)frame | flags;
    invalidate_page(page_aligned);
    vmm_write_release(cflags);
    /* Shared page-cache frames are cache-owned and mapped in several address
     * spaces — keep them out of the per-task swap set (like SysV shm). */
    if (!vma_shared)
      eviction_register_page(current_task, page_aligned, frame);
    return 0;
  }

  // Case 2: Swapped page
  if (!(pte & VMM_PRESENT) && (pte & VMM_SWAPPED)) {
    u64 new_frame = 0;
    extern int swap_in(u64 pml4_phys, u64 virtual_addr, u64 *out_physical_frame);
    if (swap_in(current_task->pml4_phys, page_aligned, &new_frame) < 0) {
      return -1;
    }

    u64 cflags;
    vmm_write_acquire(&cflags);
    u32 *slot = pf_leaf_pte_ptr(page_aligned);
    if (!slot || (*slot & VMM_PRESENT) || !(*slot & VMM_SWAPPED)) {
      vmm_write_release(cflags);
      pmm_free_frame(new_frame);
      return (slot && (*slot & VMM_PRESENT)) ? 0 : -1;
    }
    u32 flags = VMM_PRESENT | VMM_WRITABLE;
    if (*slot & VMM_USER) flags |= VMM_USER;
    *slot = (u32)new_frame | flags;
    invalidate_page(page_aligned);
    vmm_write_release(cflags);
    eviction_register_page(current_task, page_aligned, new_frame);
    return 0;
  }

  // Case 3: Copy-on-Write
  if ((error_code & PF_WRITE) && (pte & VMM_PRESENT) && (pte & VMM_COW)) {
    u64 new_frame = pmm_alloc_frame();
    if (!new_frame) {
      eviction_evict_page();
      new_frame = pmm_alloc_frame();
      if (!new_frame) return -1;
    }

    u64 cflags;
    vmm_write_acquire(&cflags);
    u32 *slot = pf_leaf_pte_ptr(page_aligned);
    if (!slot || !(*slot & VMM_PRESENT) || !(*slot & VMM_COW)) {
      vmm_write_release(cflags);
      pmm_free_frame(new_frame);
      return 0;
    }
    u32 cur = *slot;
    u32 old_frame = cur & PAGE_ENTRY_ADDRESS_MASK;
    u32 new_flags = (cur & ~PAGE_ENTRY_ADDRESS_MASK);
    new_flags &= ~VMM_COW;
    new_flags |= VMM_PRESENT | VMM_WRITABLE;

    if (pmm_get_refcount(old_frame) == 1) {
      *slot = old_frame | new_flags;
      invalidate_page(page_aligned);
      vmm_write_release(cflags);
      pmm_free_frame(new_frame);
      return 0;
    }

    memcpy((void *)(usize)(new_frame + DIRECT_MAP_BASE),
           (void *)(usize)(old_frame + DIRECT_MAP_BASE), PAGE_SIZE);
    *slot = (u32)new_frame | new_flags;
    pmm_unref_frame(old_frame);
    invalidate_page(page_aligned);
    vmm_write_release(cflags);

    eviction_unregister_page(old_frame);
    eviction_register_page(current_task, page_aligned, new_frame);
    return 0;
  }

  return -1;
}

void paging_switch_address_space(u64 pml4_phys) {
  u32 target_phys = pml4_phys ? (u32)pml4_phys : kernel_pml4_phys;
  __asm__ volatile("movl %0, %%cr3" : : "r"(target_phys) : "memory");
}

u64 paging_create_address_space(void) {
  u32 *pd = alloc_page_table();
  u32 pd_phys = table_to_phys(pd);

  u64 _vmflags;
  vmm_read_acquire(&_vmflags);

  /* Clone kernel-half entries (512-1023) */
  for (usize i = 512; i < 1024; i++) {
    pd[i] = kernel_pml4_virt[i];
  }

  /* Clone the kernel's low identity map — but ONLY enough 4 MB entries to cover
   * the kernel image (1 MB .. __kernel_end). The kernel is linked at 0x100000
   * and executes from those identity-mapped low addresses, so a user address
   * space must keep them mapped to survive a CR3 switch (the original
   * triple-fault fix). Cloning the FULL direct-map span (>=256 MB) was wrong:
   * userspace ELF executables link at 0x02000000 (32 MB), and a cloned kernel
   * *huge-page* identity PDE there shadowed the loader's 4 KB user mapping —
   * ensure_child_table treated the huge-page PDE as a page-table pointer, so the
   * binary actually executed from identity-mapped physical 0x02000000 (garbage)
   * and faulted on its first instructions. Capping the clone at the kernel image
   * leaves PD index 8 (0x02000000) free for a clean per-process user mapping. */
  extern char __kernel_end[];
  usize identity_entries = ((u32)(usize)__kernel_end >> 22) + 1;
  if (identity_entries > 512) {
    identity_entries = 512;
  }
  for (usize i = 0; i < identity_entries; i++) {
    pd[i] = kernel_pml4_virt[i];
  }

  vmm_read_release(_vmflags);
  return pd_phys;
}

u64 paging_clone_address_space(u64 src_pml4_phys) {
  u32 *src_pd = (u32 *)(usize)(src_pml4_phys + DIRECT_MAP_BASE);
  u32 *dst_pd = (u32 *)(usize)(paging_create_address_space() + DIRECT_MAP_BASE);

  u64 _vmflags;
  vmm_write_acquire(&_vmflags);

  /* Loop userspace entries. On 32-bit the user address space runs to
   * USER_SPACE_LIMIT (0xC0000000) = PD index 768, NOT 512 — the downward-growing
   * user stack lives at ~0xBFFFxxxx (PD 766-767). Cloning only 0-511 left the
   * child's stack unmapped, so fork() handed the child a FRESH ZERO stack (via
   * the lazy-alloc fault path) instead of a COW copy of the parent's: the
   * child's saved return addresses were gone, so it `ret`'d to 0 and SIGSEGV'd.
   * The direct-map huge pages at PD 512-575 (kernel) are skipped by the
   * HUGE_PAGE_FLAG guard below, so widening the bound is safe. */
  for (usize i = 0; i < 768; i++) {
    u32 pde = src_pd[i];
    if ((pde & VMM_PRESENT) && !(pde & HUGE_PAGE_FLAG)) {
      u32 *src_pt = table_from_entry(pde);
      u32 *dst_pt = alloc_page_table();
      dst_pd[i] = table_to_phys(dst_pt) | (pde & ~PAGE_ENTRY_ADDRESS_MASK);

      for (usize j = 0; j < 1024; j++) {
        u32 pte = src_pt[j];
        if (pte & VMM_PRESENT) {
          u32 frame = pte & PAGE_ENTRY_ADDRESS_MASK;
          if (frame) {
            pmm_ref_frame(frame);
          }
          /* Do not COW VMM_SHARED pages (SysV shm) — they must stay writable
           * and shared across fork. Mirrors the x86_64 path's guard (R3-14);
           * without it a shmat'd segment becomes private on first write. */
          if ((pte & VMM_WRITABLE) && !(pte & VMM_SHARED)) {
            pte = (pte & ~VMM_WRITABLE) | VMM_COW;
            src_pt[j] = pte;
          }
          dst_pt[j] = pte;
        } else if (pte & (VMM_LAZY | VMM_SWAPPED)) {
          dst_pt[j] = pte;
        }
      }
    }
  }

  vmm_write_release(_vmflags);
  return table_to_phys(dst_pd);
}

void paging_free_address_space(u64 pml4_phys) {
  if (pml4_phys == 0 || pml4_phys == kernel_pml4_phys) {
    return;
  }

  freed_tables_count = 0;
  u32 *pd = (u32 *)(usize)(pml4_phys + DIRECT_MAP_BASE);

  /* Free userspace entries (0..PD 767 — the 32-bit user range up to
   * USER_SPACE_LIMIT/0xC0000000, matching paging_clone_address_space; the user
   * stack PTs at PD 766-767 must be freed here too). The direct-map huge pages
   * at 512-575 are skipped by HUGE_PAGE_FLAG, and the shared kernel page tables
   * at PD 768+ are intentionally left untouched. */
  for (usize i = 0; i < 768; i++) {
    u32 pde = pd[i];
    if ((pde & VMM_PRESENT) && !(pde & HUGE_PAGE_FLAG)) {
      u32 *pt = table_from_entry(pde);
      u32 pt_phys = pde & PAGE_ENTRY_ADDRESS_MASK;

      for (usize j = 0; j < 1024; j++) {
        u32 pte = pt[j];
        if ((pte & VMM_PRESENT) && (pte & VMM_USER)) {
          u32 frame = pte & PAGE_ENTRY_ADDRESS_MASK;
          if (frame) {
            pmm_free_frame(frame);
            freed_tables_count++;
          }
        }
      }
      pmm_free_frame(pt_phys);
      freed_tables_count++;
    }
  }

  pmm_free_frame((u32)pml4_phys);
  freed_tables_count++;
}

void paging_mark_swapped(u64 pml4_phys, u64 vaddr) {
  u32 va = (u32)vaddr;
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);

  u32 *pd = pml4_phys ? (u32 *)(usize)(pml4_phys + DIRECT_MAP_BASE) : kernel_pml4_virt;
  u32 pde = pd[pd_index(va)];
  if ((pde & VMM_PRESENT) && !(pde & HUGE_PAGE_FLAG)) {
    u32 *pt = table_from_entry(pde);
    u32 pte = pt[pt_index(va)];
    if (pte & VMM_PRESENT) {
      pt[pt_index(va)] = (pte & ~PAGE_ENTRY_ADDRESS_MASK) & ~VMM_PRESENT;
      pt[pt_index(va)] |= VMM_SWAPPED;
    }
  }

  vmm_write_release(_vmflags);
  invalidate_page(va);
  extern void tlb_shootdown_page(u64);
  tlb_shootdown_page(vaddr);
}

int paging_test_and_clear_accessed(u64 pml4_phys, u64 vaddr) {
  u32 va = (u32)vaddr;
  u64 _vmflags;
  vmm_write_acquire(&_vmflags);

  int accessed = 0;
  u32 *pd = pml4_phys ? (u32 *)(usize)(pml4_phys + DIRECT_MAP_BASE) : kernel_pml4_virt;
  u32 pde = pd[pd_index(va)];
  if ((pde & VMM_PRESENT) && !(pde & HUGE_PAGE_FLAG)) {
    u32 *pt = table_from_entry(pde);
    u32 pte = pt[pt_index(va)];
    if (pte & VMM_PRESENT) {
      if (pte & (1 << 5)) { /* Accessed bit is bit 5 */
        accessed = 1;
        pt[pt_index(va)] &= ~(1 << 5);
      }
    }
  }

  vmm_write_release(_vmflags);
  if (accessed) {
    invalidate_page(va);
    extern void tlb_shootdown_page(u64);
    tlb_shootdown_page(vaddr);
  }
  return accessed;
}

void paging_swap_in_all_swapped(u64 pml4_phys) {
  u32 *pd = pml4_phys ? (u32 *)(usize)(pml4_phys + DIRECT_MAP_BASE) : kernel_pml4_virt;

  for (usize i = 0; i < 512; i++) {
    u32 pde = pd[i];
    if ((pde & VMM_PRESENT) && !(pde & HUGE_PAGE_FLAG)) {
      u32 *pt = table_from_entry(pde);
      for (usize j = 0; j < 1024; j++) {
        u32 pte = pt[j];
        if (!(pte & VMM_PRESENT) && (pte & VMM_SWAPPED)) {
          u32 vaddr = (i << 22) | (j << 12);
          u64 new_frame = 0;
          extern int swap_in(u64 pml4_phys, u64 virtual_addr, u64 *out_physical_frame);
          if (swap_in(pml4_phys, vaddr, &new_frame) == 0) {
            u32 flags = VMM_PRESENT | VMM_WRITABLE;
            if (pte & VMM_USER) flags |= VMM_USER;
            pt[j] = (u32)new_frame | flags;
            invalidate_page(vaddr);
            extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
            eviction_register_page(current_task, vaddr, new_frame);
          }
        }
      }
    }
  }
}

void paging_dump_entries(u64 virtual_address) {
  u32 va = (u32)virtual_address;
  u32 *pd = get_current_pml4();
  console_write("PD: 0x");
  console_write_hex32(table_to_phys(pd));
  console_write(" va: 0x");
  console_write_hex32(va);
  console_write("\n");

  u32 pde = pd[pd_index(va)];
  console_write("  PDE: 0x");
  console_write_hex32(pde);
  console_write("\n");
  if ((pde & VMM_PRESENT) && !(pde & HUGE_PAGE_FLAG)) {
    u32 *pt = table_from_entry(pde);
    u32 pte = pt[pt_index(va)];
    console_write("  PTE: 0x");
    console_write_hex32(pte);
    console_write("\n");
  }
}
