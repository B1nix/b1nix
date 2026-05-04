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
