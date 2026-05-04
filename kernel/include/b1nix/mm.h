#ifndef B1NIX_MM_H
#define B1NIX_MM_H

#include <b1nix/bootinfo.h>
#include <b1nix/types.h>

#define PAGE_SIZE 4096ULL

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER (1ULL << 2)
#define VMM_NO_EXECUTE (1ULL << 63)

void pmm_init(const struct boot_info *boot_info);
u64 pmm_alloc_frame(void);
u64 pmm_alloc_frames(usize count);
void pmm_free_frame(u64 frame);
u64 pmm_total_usable_memory(void);
u64 pmm_free_memory_estimate(void);

void kheap_init(void);
void *kmalloc(usize size);
void *kzalloc(usize size);
void kfree(void *ptr);

void vmm_init(void);
void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags);
void vmm_unmap_page(u64 virtual_address);
void vmm_remap_page(u64 virtual_address, u64 physical_address, u64 flags);
u64 vmm_direct_map_base(void);

#endif
