#include <b1nix/console.h>
#include <b1nix/sched.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/page_cache.h>
#include <b1nix/panic.h>
#include <string.h>

#define PAGE_ENTRY_ADDRESS_MASK 0x000ffffffffff000ULL
#define PAGE_TABLE_INDEX_MASK 0x1ffULL
#define HUGE_PAGE_FLAG (1ULL << 7)
#define DIRECT_MAP_SIZE (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define MMIO_MAP_BASE 0xffffa00000000000ULL
#define MMIO_MAP_SIZE (512ULL * 1024ULL * 1024ULL)

static u64 *kernel_pml4_virt;
static u64 kernel_pml4_phys;
int direct_map_ready;
static u64 mmio_next = MMIO_MAP_BASE;

extern struct task *current_task;

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

  if (frame >= 0x100000000ULL) {
    panic("vmm: page table allocated above 4GB during early boot");
  }

  u64 *table = (u64 *)(usize)frame;
  if (direct_map_ready && frame < DIRECT_MAP_SIZE) {
    table = (u64 *)(usize)(frame + DIRECT_MAP_BASE);
  }

  memset(table, 0, PAGE_SIZE);
  return table;
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

static u64 *split_huge_page(u64 *pd, usize index) {
  u64 entry = pd[index];
  u64 base = entry & PAGE_ENTRY_ADDRESS_MASK;
  u64 flags = entry & ~PAGE_ENTRY_ADDRESS_MASK;
  flags &= ~HUGE_PAGE_FLAG;

  u64 *pt = alloc_page_table();
  for (usize i = 0; i < 512; i++) {
    pt[i] = (base + i * PAGE_SIZE) | flags;
  }

  pd[index] = table_to_phys(pt) | flags;
  return pt;
}

void vmm_map_page_in_table(u64 *pml4, u64 virtual_address, u64 physical_address, u64 flags) {
  u64 *pdpt = ensure_child_table(pml4, pml4_index(virtual_address));
  u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual_address));

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

    /* Identity mapping (for transition and kernel-space execution) */
    u64 *pdpt_i = ensure_child_table(pml4, pml4_index(physical));
    u64 *pd_i = ensure_child_table(pdpt_i, pdpt_index(physical));
    pd_i[pd_index(physical)] = physical | VMM_PRESENT | VMM_WRITABLE | (1ULL << 7);
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

  console_write("paging offsets: fd_table=");
  console_write_dec((usize)&((struct task *)0)->fd_table);
  console_write(" fd_lock=");
  console_write_dec((usize)&((struct task *)0)->fd_lock);
  console_write(" pml4_phys=");
  console_write_dec((usize)&((struct task *)0)->pml4_phys);
  console_write(" vma_list=");
  console_write_dec((usize)&((struct task *)0)->vma_list);
  console_write("\n");
}

void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags) {
  if ((virtual_address & (PAGE_SIZE - 1)) != 0 ||
      (physical_address & (PAGE_SIZE - 1)) != 0) {
    panic("vmm_map_page requires page-aligned addresses");
  }

  if (kernel_pml4_virt == 0) {
    panic("vmm_map_page called before vmm_init");
  }

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
    pml4[pml4_index(virtual_address)] |= VMM_USER;
    pdpt[pdpt_index(virtual_address)] |= VMM_USER;
    pd[pd_index(virtual_address)] |= VMM_USER;
  }
  pt[pt_index(virtual_address)] =
      (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
  invalidate_page(virtual_address);

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
  }
  pt[pt_index(virtual_address)] = 0;
  invalidate_page(virtual_address);
}

void vmm_unmap_page(u64 virtual_address) {
  unmap_page_from_pml4(get_current_pml4(), virtual_address);
}

void paging_unmap_page_from_space(u64 pml4_phys, u64 virtual_address) {
  u64 *pml4 = pml4_phys ? (u64 *)(usize)(pml4_phys + DIRECT_MAP_BASE)
                         : kernel_pml4_virt;
  unmap_page_from_pml4(pml4, virtual_address);
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
    pt[pt_index(virtual_address)] =
        (pte & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
  } else if (pte & (VMM_LAZY | VMM_SWAPPED)) {
    // For non-present pages, update the saved flags
    pt[pt_index(virtual_address)] = (pte & PAGE_ENTRY_ADDRESS_MASK) | flags;
  }

  invalidate_page(virtual_address);
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
  u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual_address));
  u64 *pt = ensure_child_table(pd, pd_index(virtual_address));

  // Set a non-present entry with LAZY flag so we know it's a lazy page
  pt[pt_index(virtual_address)] = VMM_LAZY;
  invalidate_page(virtual_address);
}

// Handle page faults for demand paging and swap
int vmm_handle_page_fault(u64 fault_addr, u64 error_code) {

  u64 page_aligned = fault_addr & ~(PAGE_SIZE - 1);

  if (!is_canonical(fault_addr)) {
    panic("Non-canonical address fault!");
  }

  // Lazy Allocation for User Heap/Mmap region
  if (!(error_code & PF_PRESENT) && fault_addr >= 0x40000000 &&
      fault_addr < 0x00007FFFFFFFFFFF) {
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      extern void eviction_evict_page(void);
      eviction_evict_page();
      frame = pmm_alloc_frame();
      if (!frame) {
        panic("OOM during lazy page allocation!");
      }
    }

    // IMPORTANT: Zero the frame before giving it to user space!
    void *new_frame_virt = (void *)((uint64_t)frame + DIRECT_MAP_BASE);
    memset(new_frame_virt, 0, PAGE_SIZE);

    // Map it: Present, Read/Write, User accessible
    vmm_map_page(page_aligned, frame, VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    return 0; // Successfully resolved!
  }

  // Get the page table entry
  u64 *pml4 = get_current_pml4();
  u64 pml4e = pml4[pml4_index(page_aligned)];
  if ((pml4e & VMM_PRESENT) == 0)
    return -1;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(page_aligned)];
  if ((pdpte & VMM_PRESENT) == 0)
    return -1;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(page_aligned)];
  if ((pde & VMM_PRESENT) == 0)
    return -1;

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(page_aligned)];

  // Case 1: Lazy page (marked with VMM_LAZY flag, not present)
  if (!(pte & VMM_PRESENT) && (pte & VMM_LAZY)) {
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      extern void eviction_evict_page(void);
      eviction_evict_page();
      frame = pmm_alloc_frame();
      if (!frame) {
        // Try to swap something out to free memory
        console_write("pf: OOM during lazy allocation, swap failed\n");
        return -1;
      }
    }

    // Zero the frame
    void *new_frame_virt = (void *)((uint64_t)frame + DIRECT_MAP_BASE);
    memset(new_frame_virt, 0, PAGE_SIZE);

    // If file-backed, load from page cache or VFS
    struct vm_area *vma = current_task->vma_list;
    while (vma) {
      if (page_aligned >= vma->start && page_aligned < vma->end) {
        if (vma->node && vma->node->inode) {
          u64 file_offset = vma->offset + (page_aligned - vma->start);
          u64 file_page = file_offset & ~(PAGE_SIZE - 1);
          
          if (vma->node->inode->type == VFS_FILE) {
            struct page_cache_entry *page = page_cache_get_page(vma->node->inode, file_page);
            if (page) {
              // Use existing page cache frame (sharing!)
              pmm_free_frame(frame); // Free the freshly allocated frame
              frame = page->frame;
              pmm_ref_frame(frame);  // VMA references it
              page_cache_put_page(page);
            } else {
              // Miss in page cache, read into the fresh frame and add to cache
              if (vma->node->inode->read_cb) {
                isize res = vma->node->inode->read_cb(vma->node, file_page, (char *)new_frame_virt, PAGE_SIZE, 0);
                if (res >= 0) {
                  if (page_cache_add_page(vma->node->inode, file_page, frame) == 0) {
                    pmm_ref_frame(frame); // Cache has a reference, VMA has a reference
                  }
                } else {
                  pmm_free_frame(frame);
                  return -1;
                }
              }
            }
          } else {
            // Not a file (e.g. device node mapped lazily), just call read_cb
            if (vma->node->inode->read_cb) {
              isize res = vma->node->inode->read_cb(vma->node, file_offset, (char *)new_frame_virt, PAGE_SIZE, 0);
              if (res < 0) {
                pmm_free_frame(frame);
                return -1;
              }
            }
          }
        }
        break;
      }
      vma = vma->next;
    }

    // Build proper flags from saved flags
    u64 flags = VMM_PRESENT | VMM_WRITABLE;
    if (pte & VMM_USER)
      flags |= VMM_USER;

    pt[pt_index(page_aligned)] = frame | flags;
    invalidate_page(page_aligned);

    extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
    eviction_register_page(current_task, page_aligned, frame);
    return 0;
  }

  // Case 2: Swapped page (custom bit stored in non-present entry)
  if (!(pte & VMM_PRESENT) && (pte & VMM_SWAPPED)) {
    u64 new_frame = 0;
    if (swap_in(page_aligned, &new_frame) < 0) {
      console_write("pf: swap in failed for 0x");
      console_write_hex64(page_aligned);
      console_write("\n");
      return -1;
    }

    // Build flags from saved bits
    u64 flags = VMM_PRESENT | VMM_WRITABLE;
    if (pte & VMM_USER)
      flags |= VMM_USER;
    if (pte & VMM_NO_EXECUTE)
      flags |= VMM_NO_EXECUTE;

    pt[pt_index(page_aligned)] = new_frame | flags;
    invalidate_page(page_aligned);

    extern void eviction_register_page(struct task *task, u64 vaddr, u64 frame);
    eviction_register_page(current_task, page_aligned, new_frame);
    return 0;
  }

  // Case 3: Copy-on-Write (write to a cloned private page)
  if ((error_code & PF_WRITE) && (pte & VMM_PRESENT) && (pte & VMM_COW)) {
    u64 old_frame = pte & PAGE_ENTRY_ADDRESS_MASK;
    u64 new_frame = pmm_alloc_frame();
    if (!new_frame)
      return -1;

    memcpy((void *)(usize)(new_frame + DIRECT_MAP_BASE),
           (void *)(usize)(old_frame + DIRECT_MAP_BASE), PAGE_SIZE);

    u64 new_flags = (pte & ~PAGE_ENTRY_ADDRESS_MASK);
    new_flags &= ~VMM_COW;
    new_flags |= VMM_PRESENT | VMM_WRITABLE;
    pt[pt_index(page_aligned)] = new_frame | new_flags;

    if (pmm_get_refcount(old_frame) > 1) {
      pmm_unref_frame(old_frame);
    }
    invalidate_page(page_aligned);
    return 0;
  }

  return -1; // Unhandled
}

u64 paging_create_address_space(void) {
  u64 *pml4 = alloc_page_table();
  u64 pml4_phys = table_to_phys(pml4);
  console_write("paging_create_address_space: pml4=0x");
  console_write_hex64((u64)pml4);
  console_write(" phys=0x");
  console_write_hex64(pml4_phys);
  console_write("\n");

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

  return pml4_phys;
}

static void clone_table(u64 *src_table, u64 *dst_table, int level) {
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
        if (frame) {
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
  console_write("paging_clone_address_space: src_phys=0x");
  console_write_hex64(src_pml4_phys);
  console_write(" dst_phys=0x");
  console_write_hex64(dst_pml4_phys);
  console_write("\n");

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

  return dst_pml4_phys;
}

void paging_mark_swapped(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT)) return;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT)) return;

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(vaddr)];

  if (pte & VMM_PRESENT) {
    u64 flags = pte & ~PAGE_ENTRY_ADDRESS_MASK;
    flags &= ~VMM_PRESENT;
    flags |= VMM_SWAPPED;
    pt[pt_index(vaddr)] = flags;
    invalidate_page(vaddr);
  }
}

int paging_test_and_clear_accessed(u64 pml4_phys, u64 vaddr) {
  u64 *pml4 = (u64 *)(usize)(pml4_phys ? (pml4_phys + DIRECT_MAP_BASE) : (u64)(usize)kernel_pml4_virt);
  u64 pml4e = pml4[pml4_index(vaddr)];
  if (!(pml4e & VMM_PRESENT)) return 0;

  u64 *pdpt = table_from_entry(pml4e);
  u64 pdpte = pdpt[pdpt_index(vaddr)];
  if (!(pdpte & VMM_PRESENT)) return 0;

  u64 *pd = table_from_entry(pdpte);
  u64 pde = pd[pd_index(vaddr)];
  if (!(pde & VMM_PRESENT)) return 0;

  u64 *pt = table_from_entry(pde);
  u64 pte = pt[pt_index(vaddr)];

  if (pte & VMM_ACCESSED) {
    pt[pt_index(vaddr)] = pte & ~VMM_ACCESSED;
    invalidate_page(vaddr);
    return 1;
  }
  return 0;
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

