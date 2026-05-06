#include <string.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>

#define PAGE_ENTRY_ADDRESS_MASK 0x000ffffffffff000ULL
#define PAGE_TABLE_INDEX_MASK 0x1ffULL
#define HUGE_PAGE_FLAG (1ULL << 7)
#define DIRECT_MAP_BASE 0xffff800000000000ULL
#define DIRECT_MAP_SIZE (4ULL * 1024ULL * 1024ULL * 1024ULL)

static u64 *kernel_pml4;
static int direct_map_ready;

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
	u64 phys = entry & PAGE_ENTRY_ADDRESS_MASK;
	if (direct_map_ready && phys < DIRECT_MAP_SIZE) {
		return (u64 *)(usize)(phys + DIRECT_MAP_BASE);
	}
	return (u64 *)(usize)phys;
}

static u64 *alloc_page_table(void)
{
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

static u64 *ensure_child_table(u64 *parent, usize index)
{
	if ((parent[index] & VMM_PRESENT) == 0) {
		u64 *child = alloc_page_table();
		u64 phys_child = (u64)(usize)child;
		if (phys_child >= DIRECT_MAP_BASE) {
			phys_child -= DIRECT_MAP_BASE;
		}

		parent[index] = phys_child | VMM_PRESENT | VMM_WRITABLE;
		return child;
	}

	return table_from_entry(parent[index]);
}

void vmm_init(void)
{
	/* Get physical address of PML4 */
	u64 phys_pml4 = read_cr3() & PAGE_ENTRY_ADDRESS_MASK;
	kernel_pml4 = (u64 *)(usize)phys_pml4;
	direct_map_ready = 0;

	console_write("vmm: mapping direct map with huge pages...\n");

	/* Map the direct map using 2MB huge pages for efficiency */
	for (u64 physical = 0; physical < DIRECT_MAP_SIZE; physical += 0x200000ULL) {
		u64 virtual = DIRECT_MAP_BASE + physical;
		
		u64 *pdpt = ensure_child_table(kernel_pml4, pml4_index(virtual));
		u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual));
		
		/* Set 2MB huge page entry */
		pd[pd_index(virtual)] = physical | VMM_PRESENT | VMM_WRITABLE | (1ULL << 7); /* Bit 7 is PS (Page Size) */
		invalidate_page(virtual);
	}

	console_write("vmm: direct map 0x");
	console_write_hex64(DIRECT_MAP_BASE);
	console_write("-0x");
	console_write_hex64(DIRECT_MAP_BASE + DIRECT_MAP_SIZE);
	console_write("\n");

	/* Now that direct map is ready, we can use virtual addresses for the PML4 */
	direct_map_ready = 1;
	kernel_pml4 = (u64 *)(usize)(phys_pml4 + DIRECT_MAP_BASE);
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

// Mark a page as lazy (will allocate on first access)
void vmm_set_lazy(u64 virtual_address)
{
	if ((virtual_address & (PAGE_SIZE - 1)) != 0) return;

	u64 *pdpt = ensure_child_table(kernel_pml4, pml4_index(virtual_address));
	u64 *pd = ensure_child_table(pdpt, pdpt_index(virtual_address));
	u64 *pt = ensure_child_table(pd, pd_index(virtual_address));

	// Set a non-present entry with LAZY flag so we know it's a lazy page
	pt[pt_index(virtual_address)] = VMM_LAZY;
	invalidate_page(virtual_address);
}

// Handle page faults for demand paging and swap
int vmm_handle_page_fault(u64 fault_addr, u64 error_code)
{
	u64 page_aligned = fault_addr & ~(PAGE_SIZE - 1);
	
	// Get the page table entry
	u64 pml4e = kernel_pml4[pml4_index(page_aligned)];
	if ((pml4e & VMM_PRESENT) == 0) return -1;
	
	u64 *pdpt = table_from_entry(pml4e);
	u64 pdpte = pdpt[pdpt_index(page_aligned)];
	if ((pdpte & VMM_PRESENT) == 0) return -1;
	
	u64 *pd = table_from_entry(pdpte);
	u64 pde = pd[pd_index(page_aligned)];
	if ((pde & VMM_PRESENT) == 0) return -1;
	
	u64 *pt = table_from_entry(pde);
	u64 pte = pt[pt_index(page_aligned)];

	// Case 1: Lazy page (marked with VMM_LAZY flag, not present)
	if (!(pte & VMM_PRESENT) && (pte & VMM_LAZY)) {
		u64 frame = pmm_alloc_frame();
		if (!frame) {
			// Try to swap something out to free memory
			console_write("pf: OOM during lazy allocation, trying swap\n");
			return -1;
		}
		
		// Build proper flags from saved flags
		u64 flags = VMM_PRESENT | VMM_WRITABLE;
		if (pte & VMM_USER) flags |= VMM_USER;
		
		pt[pt_index(page_aligned)] = frame | flags;
		invalidate_page(page_aligned);
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
		if (pte & VMM_USER) flags |= VMM_USER;
		if (pte & VMM_NO_EXECUTE) flags |= VMM_NO_EXECUTE;
		
		pt[pt_index(page_aligned)] = new_frame | flags;
		invalidate_page(page_aligned);
		return 0;
	}

	// Case 3: Copy-on-Write (write to a read-only shared page)
	if ((error_code & PF_WRITE) && (pte & VMM_PRESENT) && !(pte & VMM_WRITABLE)) {
		u64 old_frame = pte & PAGE_ENTRY_ADDRESS_MASK;
		u64 new_frame = pmm_alloc_frame();
		if (!new_frame) return -1;
		
		// Copy old page contents
		memcpy((void *)(usize)new_frame, (void *)(usize)old_frame, PAGE_SIZE);
		
		pt[pt_index(page_aligned)] = new_frame | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
		invalidate_page(page_aligned);
		return 0;
	}
	
	return -1; // Unhandled
}
