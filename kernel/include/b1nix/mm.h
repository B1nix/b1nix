#ifndef B1NIX_MM_H
#define B1NIX_MM_H

#include <b1nix/bootinfo.h>
#include <b1nix/types.h>

#define PAGE_SIZE 4096ULL
#define DIRECT_MAP_BASE 0xffff800000000000ULL
#define KHEAP_START 0xffffc00000000000ULL

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER (1ULL << 2)
#define VMM_ACCESSED (1ULL << 5)
#define VMM_DIRTY (1ULL << 6)
#define VMM_PWT (1ULL << 3)
#define VMM_PCD (1ULL << 4)
#define VMM_NO_EXECUTE (1ULL << 63)

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
#define MAP_FAILED ((void *)-1)

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
u64 pmm_alloc_frame(void);
u64 pmm_alloc_frames(usize count);
void pmm_ref_frame(u64 frame);
void pmm_unref_frame(u64 frame);
void pmm_free_frame(u64 frame);
u16 pmm_get_refcount(u64 frame);
u64 pmm_total_usable_memory(void);
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
void paging_switch_address_space(u64 pml4_phys);

// Demand Paging / Swap
void vmm_set_lazy(u64 virtual_address);
int vmm_handle_page_fault(u64 fault_addr, u64 error_code);
void vmm_set_swap_device(struct block_device *dev);

// Swap
int swap_init(void);
int swap_out(u64 virtual_addr, u64 physical_frame);
int swap_in(u64 virtual_addr, u64 *out_physical_frame);

#endif
