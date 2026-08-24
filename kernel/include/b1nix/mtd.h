/*
 * MTD — raw flash, addressed as flash rather than as a disk.
 *
 * A flash chip is not a disk: a write can only clear bits, so a region has to
 * be erased (back to all-ones) before it can be written again, and erasing
 * works in blocks that are far larger than a sector. That is the whole reason
 * MTD exists as a separate interface — hiding it behind a block device means
 * hiding the erase, and a caller that cannot erase cannot use flash correctly.
 *
 * The device here is CFI NOR, which is what QEMU emulates on x86_64 through
 * `-drive if=pflash`. There is no NAND on any b1nix target, so the NAND-only
 * tools (nanddump, nandwrite, and the out-of-band data they exist for) remain
 * unbuilt: an emulated NAND would be b1nix testing itself.
 */
#ifndef B1NIX_MTD_H
#define B1NIX_MTD_H

#include <b1nix/types.h>

#define MTD_MAX_DEVICES 2

/* MTD_NORFLASH, as the MEMGETINFO ioctl reports it. */
#define MTD_TYPE_NORFLASH 3
/* MTD_CAP_NORFLASH: readable, writeable, erasable. */
#define MTD_CAP_NORFLASH 0x400

/* The layout MEMGETINFO returns, field for field as Linux defines it -- and
 * NOT packed, which matters more than it looks. Linux's struct mtd_info_user
 * is an ordinary struct, so the compiler pads the one-byte `type` out to the
 * alignment of the u32 that follows. Packing it here shifted every later field
 * by three bytes: flash_eraseall read the erase-block size as 0, printed
 * "Erasing 0 Kibyte" and then asked the kernel to erase nonsense. */
struct mtd_info_user {
	u8 type;
	u32 flags;
	u32 size;
	u32 erasesize;
	u32 writesize;
	u32 oobsize;
	u64 padding;
};

/* MEMERASE's argument. */
struct erase_info_user {
	u32 start;
	u32 length;
} __attribute__((packed));

struct mtd_device {
	const char *name;
	volatile u8 *base;   /* where the chip is mapped */
	u32 size;
	u32 erase_size;
	int present;
};

/* Probe for a CFI NOR chip and publish /dev/mtd0 and /dev/mtdblock0.
 * Probing WRITES a CFI query command, so it happens only when asked for with
 * b1nix.mtd on the command line: on real hardware that address space belongs
 * to the firmware. */
void mtd_init(void);

/* Re-create /dev/mtdN. Called whenever devtmpfs mounts, because /dev is
 * rebuilt from scratch there and a node made at probe time would be lost. */
void mtd_create_dev_nodes(void);

struct mtd_device *mtd_device_at(unsigned index);
int mtd_read(struct mtd_device *d, u32 offset, void *buf, u32 len);
int mtd_write(struct mtd_device *d, u32 offset, const void *buf, u32 len);
int mtd_erase(struct mtd_device *d, u32 offset, u32 len);

#endif /* B1NIX_MTD_H */
