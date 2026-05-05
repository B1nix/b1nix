#include <b1nix/mm.h>
#include <b1nix/console.h>

void vmm_init(void)
{
	console_write("aarch64: vmm_init (stub)\n");
	// AArch64 QEMU virt boots with MMU disabled. Since we are running a minimal
	// OS and haven't fully implemented page tables for AArch64, we will run
	// physically addressed for now, or just provide stubs.
	// For M13, this is acceptable.
}

void vmm_map_page(u64 virtual_address, u64 physical_address, u64 flags)
{
	(void)virtual_address;
	(void)physical_address;
	(void)flags;
}

void vmm_unmap_page(u64 virtual_address)
{
	(void)virtual_address;
}

void vmm_remap_page(u64 virtual_address, u64 physical_address, u64 flags)
{
	vmm_unmap_page(virtual_address);
	vmm_map_page(virtual_address, physical_address, flags);
}

u64 vmm_direct_map_base(void)
{
	return 0;
}

void vmm_set_lazy(u64 virtual_address)
{
	(void)virtual_address;
}

int vmm_handle_page_fault(u64 fault_addr, u64 error_code)
{
	(void)fault_addr;
	(void)error_code;
	return -1;
}
