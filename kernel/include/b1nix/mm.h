#ifndef B1NIX_MM_H
#define B1NIX_MM_H

#include <b1nix/bootinfo.h>
#include <b1nix/types.h>

#define PAGE_SIZE 4096ULL

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER (1ULL << 2)
#define VMM_NO_EXECUTE (1ULL << 63)

// Page fault error code bits
#define PF_PRESENT  (1ULL << 0)
#define PF_WRITE    (1ULL << 1)
#define PF_USER     (1ULL << 2)
#define PF_RESERVED (1ULL << 3)
#define PF_INSTR    (1ULL << 4)

// Swap / Demand Paging
#define VMM_SWAPPED (1ULL << 9)  // Custom flag: page is swapped out
#define VMM_LAZY    (1ULL << 10) // Custom flag: lazy allocation
#define SWAP_BLOCK_SIZE PAGE_SIZE

struct block_device;

struct swap_slot {
    u32 block_lba;   // LBA on swap device
    u32 inode_num;   // For file-backed swap
    int present;
};

void pmm_init(const struct boot_info *boot_info);
u64 pmm_alloc_frame(void);
u64 pmm_alloc_frames(usize count);
void pmm_free_frame(u64 frame);
u64 pmm_total_usable_memory(void);
u64 pmm_free_memory_estimate(void);
usize pmm_free_frame_count(void);

void kheap_init(void);
void *kmalloc(usize size);
void *kzalloc(usize size);
void kfree(void *ptr);

void vmm_init(void);
void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags);
void vmm_unmap_page(u64 virtual_address);
void vmm_remap_page(u64 virtual_address, u64 physical_address, u64 flags);
u64 vmm_direct_map_base(void);

// Demand Paging / Swap
void vmm_set_lazy(u64 virtual_address);
int vmm_handle_page_fault(u64 fault_addr, u64 error_code);
void vmm_set_swap_device(struct block_device *dev);

// Swap
int swap_init(void);
int swap_out(u64 virtual_addr, u64 physical_frame);
int swap_in(u64 virtual_addr, u64 *out_physical_frame);

#endif
