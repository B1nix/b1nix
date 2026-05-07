#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/panic.h>

static struct boot_info global_bootinfo;

void bootinfo_init_from_fdt(u64 dtb_address)
{
	(void)dtb_address;
	// For this minimal kernel, instead of full FDT parsing,
	// we hardcode the memory map of QEMU virt machine.
	// virt machine starts RAM at 0x40000000.
	// We'll assume 128MB of RAM for now.
	// Actually, let's provide a reasonable size, e.g., 256MB.
	
	global_bootinfo.memory_region_count = 1;
	global_bootinfo.memory_regions[0].base = 0x40000000;
	global_bootinfo.memory_regions[0].length = 256 * 1024 * 1024;
	global_bootinfo.memory_regions[0].type = BOOT_MEMORY_AVAILABLE;
	
	global_bootinfo.has_framebuffer = 0; // Disable framebuffer for now

	console_write("aarch64: initialized bootinfo (hardcoded QEMU virt)\n");
}

const struct boot_info *bootinfo_get(void)
{
	return &global_bootinfo;
}
