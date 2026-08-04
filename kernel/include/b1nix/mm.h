#ifndef B1NIX_MM_H
#define B1NIX_MM_H

#include <b1nix/bootinfo.h>
#include <b1nix/types.h>

#define PAGE_SIZE 4096ULL

/* KERNEL_VMA: the offset between a kernel symbol's virtual address and its
 * physical load address. Both kernels are higher-half (linked high, loaded at
 * 1M); subtract KERNEL_VMA to turn a kernel symbol into a physical address.
 * x86_64 uses the canonical -2GB window; x86 (32-bit) links into the direct map
 * (0x80000000) — the kernel's symbols are simply the direct-map view of its own
 * physical image, so no separate high window is needed. */
#ifdef __x86_64__
#define KERNEL_VMA 0xFFFFFFFF80000000ULL
#else
#define KERNEL_VMA 0x80000000ULL
#endif

#ifdef __x86_64__
#define DIRECT_MAP_BASE 0xffff800000000000ULL
#define DIRECT_MAP_MIN  (4ULL * 1024ULL * 1024ULL * 1024ULL)
/* The current 4-level page-table layout reserves [ffff8000_00000000,
 * ffffc000_00000000) for the physical direct map: 64 TiB. Only the
 * firmware-reported top of RAM is mapped at boot, so using the full window as
 * the ceiling does not add page tables or boot work on smaller machines. */
#define DIRECT_MAP_MAX  (64ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)
#define KHEAP_START 0xffffc00000000000ULL
#else
#define DIRECT_MAP_BASE 0x80000000ULL
#define DIRECT_MAP_MIN  (256ULL * 1024ULL * 1024ULL)
#define DIRECT_MAP_MAX  (1024ULL * 1024ULL * 1024ULL)
#define KHEAP_START     0xc0000000ULL
#endif

/* Physical memory below DIRECT_MAP_SIZE is identity-accessible via the direct
 * map (phys + DIRECT_MAP_BASE). The pmm must never hand out a frame at or above
 * this limit, and vmm_init maps exactly this range — keep the two in lockstep.
 *
 * DIRECT_MAP_SIZE is now a runtime value sized to the Multiboot2 top-of-RAM
 * at pmm_init, clamped to [DIRECT_MAP_MIN, DIRECT_MAP_MAX] and aligned to a
 * 2 MiB boundary. The macro forwards to the global so existing call sites
 * (`if (x < DIRECT_MAP_SIZE) ...`) keep compiling unchanged.
 *
 * MIN is 4 GiB (64-bit) or 256 MiB (32-bit). PCI MMIO BARs (AHCI ABAR ~ 0xFEBD6000, etc.) sit in the
 * 32-bit PCI hole and several drivers access them as
 * `phys + DIRECT_MAP_BASE` directly. Below 4 GiB those would page-fault.
 * (The fully clean alternative — vmm_map_mmio() in every driver — is a
 * separate cleanup; for now the floor keeps the legacy direct-map MMIO
 * pattern working.) */
extern u64 g_direct_map_size;
#define DIRECT_MAP_SIZE (g_direct_map_size)

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER (1ULL << 2)
#define VMM_ACCESSED (1ULL << 5)
#define VMM_DIRTY (1ULL << 6)
#define VMM_SHARED (1ULL << 8)
#define VMM_PWT (1ULL << 3)
#define VMM_PCD (1ULL << 4)
/* Leaf-PTE PAT bit (bit 7). Only ever set on a 4 KiB PTE — in a PDE/PDPTE the
 * same bit is PS (huge page), so VMM_WC must never reach a directory entry. */
#define VMM_PAT (1ULL << 7)
/* M98: write-combining. Selects PAT slot 5, which pat_init_cpu() programs to
 * WC (see kernel/include/b1nix/memtype.h). Falls back to write-through — the
 * reset meaning of slot 5 — on a CPU without PAT, which is correct but slow
 * rather than wrong. */
#define VMM_WC (VMM_PAT | VMM_PWT)

#ifdef __x86_64__
#define VMM_NO_EXECUTE (1ULL << 63)
#else
#define VMM_NO_EXECUTE 0ULL
#endif

/* mmap protections */
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

/* mmap flags */
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED 0x10

/* M72: msync flags (Linux ABI). */
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4
/* MAP_NORESERVE: do not reserve swap/commit up front. b1nix anonymous mappings
 * already commit lazily on first touch (the page-fault handler zero-fills the
 * faulting page), so this flag selects exactly that existing behavior — we just
 * accept it instead of rejecting it. No reservation accounting is faked. */
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void *)-1)

/* madvise() advice values (Linux ABI numbers). */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8
/* Advisory hints accepted as a legal POSIX no-op (b1nix has no fork-inherit
 * control or transparent hugepages — the kernel just acknowledges them). */
#define MADV_DONTFORK   10
#define MADV_DOFORK     11
#define MADV_HUGEPAGE   14
#define MADV_NOHUGEPAGE 15

// Page fault error code bits
#define PF_PRESENT (1ULL << 0)
#define PF_WRITE (1ULL << 1)
#define PF_USER (1ULL << 2)
#define PF_RESERVED (1ULL << 3)
#define PF_INSTR (1ULL << 4)

// Swap / Demand Paging
#define VMM_SWAPPED (1ULL << 9) // Custom flag: page is swapped out
#define VMM_LAZY (1ULL << 10)   // Custom flag: lazy allocation
#define VMM_COW (1ULL << 11)    // Custom flag: copy-on-write mapping
#define SWAP_BLOCK_SIZE PAGE_SIZE

struct block_device;

struct swap_slot {
  u32 block_lba; // LBA on swap device
  u32 inode_num; // For file-backed swap
  int present;
};

void pmm_init(const struct boot_info *boot_info);
/* Variant B — start the background reclaim (kswapd) kernel thread. Call once the
 * scheduler and page cache are up. */
void kswapd_init(void);
u64 pmm_alloc_frame(void);
u64 pmm_alloc_frames(usize count);

/* M99: `count` contiguous frames whose last byte is <= limit — the allocation a
 * DMA bounce buffer needs when a device's address window is narrower than the
 * memory it was asked to reach. Returns 0 when nothing under the ceiling is
 * free. Walks the used-bitmap, so it is O(limit/PAGE_SIZE): a slow-path call. */
u64 pmm_alloc_frames_below(usize count, u64 limit);
/* Physical address of the single shared zero page (zero-page dedup). Reserved
 * for the kernel's lifetime; pmm_free_frame() ignores it. */
u64 pmm_zero_page(void);
void pmm_ref_frame(u64 frame);
void pmm_unref_frame(u64 frame);
void pmm_free_frame(u64 frame);
u16 pmm_get_refcount(u64 frame);
u64 pmm_total_usable_memory(void);
u64 pmm_phys_total_memory(void);
u64 pmm_free_memory_estimate(void);
usize pmm_free_frame_count(void);

void kheap_init(void);
void kheap_use_direct_map(void);
/* Fault-dump helper: print which general-heap block (if any) an address falls
 * in, and whether that block is live or freed. */
void kheap_describe(u64 addr, const char *label);
void *kmalloc(usize size);
void *kzalloc(usize size);
void kfree(void *ptr);

void vmm_init(void);
void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags);
void *vmm_map_mmio(u64 physical_address, usize size, u64 flags);
void vmm_unmap_page(u64 virtual_address);
void paging_unmap_page_from_space(u64 pml4_phys, u64 virtual_address);
void vmm_remap_page(u64 virtual_address, u64 physical_address, u64 flags);
u64 vmm_direct_map_base(void);
u64 vmm_virt_to_phys(void *ptr);
extern int direct_map_ready;
void paging_dump_entries(u64 virtual_address);
/* Raw leaf page-table entry backing `vaddr` in the current address space, or 0
 * when nothing 4 KiB-mapped is there. Used to verify memory-type bits (M98). */
u64 paging_leaf_pte(u64 virtual_address);

/* M100: install the kernel-half page-table path for a range without mapping a
 * page into it, so the window exists in every address space created later (new
 * address spaces copy PML4 entries by value — an entry created afterwards is
 * private to whichever process created it). */
int paging_reserve_kernel_path(u64 virtual_address, u64 size);

/* The PML4 entry `pml4_phys` holds for an address, 0 when absent. Lets a test
 * assert that a kernel window is shared rather than assuming it. */
u64 paging_pml4_entry_in(u64 pml4_phys, u64 virtual_address);
u64 paging_pml4_entry_current(u64 virtual_address);

// M2 Aliases / Helpers
void paging_map_page(u64 virtual_address, u64 physical_address, u64 flags);
void paging_unmap_page(u64 virtual_address);
void paging_mprotect_page(u64 virtual_address, u64 flags);
u64 paging_create_address_space(void);
u64 paging_clone_address_space(u64 src_pml4_phys);
void paging_free_address_space(u64 pml4_phys);
void paging_switch_address_space(u64 pml4_phys);

/* Physical frame backing `vaddr` in the address space rooted at `pml4_phys`,
 * or 0 if the page is not present. Used to measure a task's real resident set
 * (/proc/<pid>/statm) without touching the current address space. */
u64 paging_user_frame(u64 pml4_phys, u64 vaddr);
/* Physical address backing a user VA, resolving huge (1 GiB / 2 MiB) mappings
 * too. Readers of another task's memory must use this rather than
 * paging_user_frame, which reports a huge-mapped address as absent. */
u64 paging_user_phys(u64 pml4_phys, u64 vaddr);

// Demand Paging / Swap
void vmm_set_lazy(u64 virtual_address);
int vmm_handle_page_fault(u64 fault_addr, u64 error_code);
void vmm_set_swap_device(struct block_device *dev);

// Swap. Slot-bitmap design: swap_out writes a frame to a freshly allocated slot
// and returns its index; the caller stores that index in the page's VMM_SWAPPED
// PTE. swap_in reads the page at `slot` (decoded from the faulting PTE) and frees
// the slot. No (pml4,vaddr) reverse-map table.
int swap_init(void);
int swap_active(void);
/* Slot accounting (one slot == one page): 0 on success, -1 if swap is off. */
int swap_stats(u64 *out_total_slots, u64 *out_used_slots);
/* swapoff(2) backing: 0 on success, -1 if no device, -2 if slots are still in
 * use (the caller must page them back in first). */
int swap_detach(void);
/* Page every swapped-out page of an address space back into RAM. */
void paging_swap_in_all_swapped(u64 pml4_phys);
int swap_out(u64 physical_frame);
int swap_in(u32 slot, u64 *out_physical_frame);
void swap_free_slot_index(u32 slot);
void swap_free_all_slots(u64 pml4_phys);

/* mlock(2) backing: a locked range is skipped by the CLOCK eviction scan, so
 * its pages are never swapped out while the owning task lives. Returns -1 when
 * the (small) range table is full — mlock reports ENOMEM, exactly as Linux does
 * when it cannot honour the lock. */
struct task;
int eviction_lock_range(struct task *task, u64 start, u64 end);
void eviction_unlock_range(struct task *task, u64 start, u64 end);
void eviction_unlock_all(struct task *task);

#endif
