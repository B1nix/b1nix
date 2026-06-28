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

// M2 Aliases / Helpers
void paging_map_page(u64 virtual_address, u64 physical_address, u64 flags);
void paging_unmap_page(u64 virtual_address);
void paging_mprotect_page(u64 virtual_address, u64 flags);
u64 paging_create_address_space(void);
u64 paging_clone_address_space(u64 src_pml4_phys);
void paging_free_address_space(u64 pml4_phys);
void paging_switch_address_space(u64 pml4_phys);

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
int swap_out(u64 physical_frame);
int swap_in(u32 slot, u64 *out_physical_frame);
void swap_free_slot_index(u32 slot);
void swap_free_all_slots(u64 pml4_phys);

#endif
