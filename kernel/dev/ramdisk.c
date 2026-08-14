#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/ramdisk.h>
#include <string.h>

static u64 ramdisk_phys_addr = 0;
static u64 ramdisk_size = 0;

static int ramdisk_read_blocks(struct block_device *dev, u64 lba, u32 count, void *buffer) {
	(void)dev;
	u64 offset = lba * 512;
	u64 size = (u64)count * 512;
	if (offset + size > ramdisk_size) {
		return -1;
	}
	memcpy(buffer, (void *)(usize)(ramdisk_phys_addr + DIRECT_MAP_BASE + offset), size);
	return 0;
}

static int ramdisk_write_blocks(struct block_device *dev, u64 lba, u32 count, const void *buffer) {
	(void)dev;
	u64 offset = lba * 512;
	u64 size = (u64)count * 512;
	if (offset + size > ramdisk_size) {
		return -1;
	}
	memcpy((void *)(usize)(ramdisk_phys_addr + DIRECT_MAP_BASE + offset), buffer, size);
	return 0;
}

static struct block_device ramdisk_dev = {
	.name = "ram0",
	.bus = BLK_BUS_MEMORY,
	.block_size = 512,
	.block_count = 0,
	.read_blocks = ramdisk_read_blocks,
	.write_blocks = ramdisk_write_blocks,
	.priv = NULL
};

void ramdisk_init(void) {
	const struct boot_info *bi = bootinfo_get();
	if (!bi->has_ramdisk) {
		return;
	}

	ramdisk_phys_addr = bi->ramdisk_addr;
	ramdisk_size = bi->ramdisk_size;
	ramdisk_dev.block_count = ramdisk_size / 512;

	blk_register(&ramdisk_dev);

	console_write("ramdisk: registered ram0 addr=0x");
	console_write_hex64(ramdisk_phys_addr);
	console_write(" size=");
	console_write_dec(ramdisk_size / (1024 * 1024));
	console_write(" MiB\n");
}
