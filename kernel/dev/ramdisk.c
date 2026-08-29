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

/* Is what the block device hands out actually the image that was packed?
 *
 * On the Xperia the ext4 mount of ram0 fails with EINVAL and the boot falls
 * back to the initramfs, which has no init and no shell. EINVAL from the mount
 * says only "this is not an ext4 superblock" — it cannot say whether the
 * bootloader put the ramdisk somewhere else, never loaded it, or loaded it fine
 * and the direct map points at the wrong physical page. All three look
 * identical from the mount's side, so report the bytes themselves: the ext4
 * magic lives at offset 1080 (superblock at 1024, s_magic at +56) and must read
 * 0xEF53.
 *
 * Callable again later because this prints early and a phone panel has scrolled
 * far past it by the time anything goes wrong — the init spawn repeats it next
 * to its own failure, where the two facts belong together.
 */
void ramdisk_report(void)
{
	static const char hex[] = "0123456789abcdef";
	const u8 *p;
	char head[16 * 2 + 1];
	u16 magic;

	if (!ramdisk_size) {
		console_write("ramdisk: none registered\n");
		return;
	}

	p = (const u8 *)(usize)(ramdisk_phys_addr + DIRECT_MAP_BASE);
	magic = (u16)(p[1080] | ((u16)p[1081] << 8));

	for (int i = 0; i < 16; i++) {
		head[i * 2] = hex[p[i] >> 4];
		head[i * 2 + 1] = hex[p[i] & 0xf];
	}
	head[sizeof(head) - 1] = '\0';

#ifdef __aarch64__
	/* Only the device-tree boot path has a loader claim to report; the
	 * Multiboot2 path carries the module bounds in the boot info itself. */
	bootinfo_report_initrd();
#endif
	console_write("ramdisk: ram0 addr=0x");
	console_write_hex64(ramdisk_phys_addr);
	console_write(" ext4 magic=0x");
	console_write_hex32(magic);
	console_write(magic == 0xEF53 ? " ok head=" : " BAD head=");
	console_write(head);
	console_write("\n");
}

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

	ramdisk_report();
}
