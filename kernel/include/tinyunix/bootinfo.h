#ifndef TINYUNIX_BOOTINFO_H
#define TINYUNIX_BOOTINFO_H

#include <tinyunix/types.h>

#define BOOTINFO_MAX_MEMORY_REGIONS 32

enum boot_memory_region_type {
	BOOT_MEMORY_AVAILABLE = 1,
	BOOT_MEMORY_RESERVED = 2,
	BOOT_MEMORY_ACPI_RECLAIMABLE = 3,
	BOOT_MEMORY_NVS = 4,
	BOOT_MEMORY_BADRAM = 5,
};

struct boot_memory_region {
	u64 base;
	u64 length;
	u32 type;
};

struct boot_framebuffer {
	u64 address;
	u32 pitch;
	u32 width;
	u32 height;
	u8 bpp;
	u8 type;
};

struct boot_info {
	struct boot_memory_region memory_regions[BOOTINFO_MAX_MEMORY_REGIONS];
	usize memory_region_count;
	struct boot_framebuffer framebuffer;
	int has_framebuffer;
};

void bootinfo_init_from_multiboot2(u32 magic, u32 info_address);
const struct boot_info *bootinfo_get(void);

#endif
