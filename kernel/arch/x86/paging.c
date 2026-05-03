#include <string.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>

#define PAGE_ENTRY_ADDRESS_MASK 0x000ffffffffff000ULL
#define PAGE_TABLE_INDEX_MASK 0x1ffULL
#define HUGE_PAGE_FLAG (1ULL << 7)
#define DIRECT_MAP_BASE 0xffff800000000000ULL
#define DIRECT_MAP_SIZE (128ULL * 1024ULL * 1024ULL)

static u64 *kernel_pml4;

static u64 read_cr3(void)
{
	u64 value;

	__asm__ volatile("movq %%cr3, %0" : "=r"(value));
	return value;
}

static void invalidate_page(u64 virtual_address)
{
	__asm__ volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

static usize pml4_index(u64 virtual_address)
{
	return (virtual_address >> 39) & PAGE_TABLE_INDEX_MASK;
}

static usize pdpt_index(u64 virtual_address)
{
	return (virtual_address >> 30) & PAGE_TABLE_INDEX_MASK;
}

static usize pd_index(u64 virtual_address)
{
	return (virtual_address >> 21) & PAGE_TABLE_INDEX_MASK;
}

static usize pt_index(u64 virtual_address)
{
	return (virtual_address >> 12) & PAGE_TABLE_INDEX_MASK;
}

static u64 *table_from_entry(u64 entry)
{
	return (u64 *)(usize)(entry & PAGE_ENTRY_ADDRESS_MASK);
}

static u64 *alloc_page_table(void)
{
	u64 frame = pmm_alloc_frame();
	u64 *table = (u64 *)(usize)frame;

	memset(table, 0, PAGE_SIZE);
	return table;
}

static u64 *ensure_child_table(u64 *parent, usize index)
{
	if ((parent[index] & VMM_PRESENT) == 0) {
		u64 *child = alloc_page_table();
		parent[index] = ((u64)(usize)child) | VMM_PRESENT | VMM_WRITABLE;
		return child;
	}

	return table_from_entry(parent[index]);
}

void vmm_init(void)
{
	kernel_pml4 = (u64 *)(usize)(read_cr3() & PAGE_ENTRY_ADDRESS_MASK);

	console_write("vmm: pml4 0x");
	console_write_hex64((u64)(usize)kernel_pml4);
	console_write("\n");

	for (u64 physical = 0; physical < DIRECT_MAP_SIZE; physical += PAGE_SIZE) {
		vmm_map_page(DIRECT_MAP_BASE + physical, physical, VMM_WRITABLE);
	}

	console_write("vmm: direct map 0x");
	console_write_hex64(DIRECT_MAP_BASE);
	console_write("-0x");
	console_write_hex64(DIRECT_MAP_BASE + DIRECT_MAP_SIZE);
	console_write("\n");
}

void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags)
{
	if ((virtual_address & (PAGE_SIZE - 1)) != 0 || (physical_address & (PAGE_SIZE - 1)) != 0) {
		panic("vmm_map_page requires page-aligned addresses");
	}

	if (kernel_pml4 == 0) {
		panic("vmm_map_page called before vmm_init");
	}

	u64 *pdpt = ensure_child_table(kernel_pml4, pml4_index(virtual_address));
	u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual_address));

	if ((pd[pd_index(virtual_address)] & HUGE_PAGE_FLAG) != 0) {
		panic("vmm_map_page cannot split huge pages yet");
	}

	u64 *pt = ensure_child_table(pd, pd_index(virtual_address));
	pt[pt_index(virtual_address)] = (physical_address & PAGE_ENTRY_ADDRESS_MASK) | flags | VMM_PRESENT;
	invalidate_page(virtual_address);
}

void vmm_unmap_page(u64 virtual_address)
{
	if ((virtual_address & (PAGE_SIZE - 1)) != 0) {
		panic("vmm_unmap_page requires page-aligned address");
	}

	u64 pml4e = kernel_pml4[pml4_index(virtual_address)];
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
		panic("vmm_unmap_page cannot unmap huge pages yet");
	}

	u64 *pt = table_from_entry(pde);
	pt[pt_index(virtual_address)] = 0;
	invalidate_page(virtual_address);
}

void vmm_remap_page(u64 virtual_address, u64 physical_address, u64 flags)
{
	vmm_unmap_page(virtual_address);
	vmm_map_page(virtual_address, physical_address, flags);
}

u64 vmm_direct_map_base(void)
{
	return DIRECT_MAP_BASE;
}
