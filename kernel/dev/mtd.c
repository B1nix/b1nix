/*
 * CFI NOR flash, and the MTD interface over it.
 *
 * The chip is memory-mapped: reading it is reading memory, and writing it is
 * sending a command sequence to the same addresses. The command set here is
 * Intel's (CFI 0x0001), which is what QEMU's pflash_cfi01 implements and what
 * `-drive if=pflash` gives an x86_64 guest.
 *
 * Two rules a flash chip imposes and this code obeys:
 *
 *   - A program can only clear bits. Writing 0xFF over 0x00 does nothing. So a
 *     region must be erased -- which sets it back to all-ones -- before it can
 *     hold something else.
 *   - Erasing works in blocks. The block size comes from the chip's own CFI
 *     table rather than from a constant here, because getting it wrong means
 *     erasing someone else's data.
 *
 * DISCOVERY. The chip is not on any bus that can be enumerated: on x86 it sits
 * just below 4 GiB, under whatever firmware flash occupies the top. Probing
 * means WRITING a query command to a physical address, so it is done only when
 * b1nix.mtd is on the command line. On real hardware that address space is the
 * firmware's, and probing it uninvited would be reckless.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/mtd.h>
#include <b1nix/errno.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <string.h>

static struct mtd_device g_mtd[MTD_MAX_DEVICES];

/* CFI / Intel command set */
#define CFI_CMD_QUERY      0x98
#define CFI_CMD_READ_ARRAY 0xFF
#define CFI_CMD_ERASE      0x20
#define CFI_CMD_CONFIRM    0xD0
#define CFI_CMD_PROGRAM    0x40
#define CFI_CMD_STATUS     0x70
#define CFI_CMD_CLEAR_SR   0x50
#define CFI_SR_READY       0x80
#define CFI_SR_ERASE_ERR   0x20
#define CFI_SR_PROGRAM_ERR 0x10

static void mtd_reset(volatile u8 *base)
{
	base[0] = CFI_CMD_READ_ARRAY;
}

/* Wait for the chip to finish, and report what it says rather than assuming it
 * worked: an erase that failed leaves the block readable but unchanged, which
 * looks exactly like success to a caller that does not ask. */
static int mtd_wait(volatile u8 *base)
{
	base[0] = CFI_CMD_STATUS;
	for (u32 spin = 0; spin < 200000000u; spin++) {
		u8 sr = base[0];

		if (!(sr & CFI_SR_READY))
			continue;
		mtd_reset(base);
		if (sr & (CFI_SR_ERASE_ERR | CFI_SR_PROGRAM_ERR))
			return -1;
		return 0;
	}
	mtd_reset(base);
	return -1;
}

/* Ask one candidate address whether a CFI chip lives there. */
static int mtd_probe_at(u64 phys, struct mtd_device *out)
{
	volatile u8 *base = vmm_map_mmio(phys, 0x1000, VMM_WRITABLE | VMM_PCD);

	if (!base)
		return -1;

	base[0x55] = CFI_CMD_QUERY;
	int is_cfi = (base[0x10] == 'Q' && base[0x11] == 'R' && base[0x12] == 'Y');
	if (!is_cfi) {
		mtd_reset(base);
		return -1;
	}

	/* Device size is 2^n bytes, at query offset 0x27. */
	u8 size_shift = base[0x27];
	if (size_shift < 12 || size_shift > 30) {
		mtd_reset(base);
		return -1;
	}
	u32 size = 1u << size_shift;

	/* Erase-block layout: the first region's block count (0x2D/0x2E, minus
	 * one) and its block size in 256-byte units (0x2F/0x30). One region is
	 * enough for a uniform chip, which is what pflash is; a chip with several
	 * regions would need the rest, and is not what any target here has. */
	u32 blocks = ((u32)base[0x2D] | ((u32)base[0x2E] << 8)) + 1;
	u32 unit = ((u32)base[0x2F] | ((u32)base[0x30] << 8));
	u32 erase = unit * 256u;

	mtd_reset(base);

	if (erase == 0 || blocks == 0 || erase > size) {
		return -1;
	}

	/* Map the whole chip now that its size is known. */
	volatile u8 *full = vmm_map_mmio(phys, size, VMM_WRITABLE | VMM_PCD);
	if (!full)
		return -1;

	out->base = full;
	out->size = size;
	out->erase_size = erase;
	out->present = 1;
	return 0;
}

int mtd_read(struct mtd_device *d, u32 offset, void *buf, u32 len)
{
	if (!d || !d->present || offset > d->size || len > d->size - offset)
		return -1;
	/* Reading is just reading: the chip is in array mode. */
	u8 *out = buf;
	for (u32 i = 0; i < len; i++)
		out[i] = d->base[offset + i];
	return 0;
}

int mtd_erase(struct mtd_device *d, u32 offset, u32 len)
{
	if (!d || !d->present || offset > d->size || len > d->size - offset)
		return -1;
	/* Erase works in blocks, so a partial block is a caller error rather
	 * than something to round silently: rounding would wipe data the caller
	 * never mentioned. */
	if (offset % d->erase_size || len % d->erase_size)
		return -1;

	for (u32 at = offset; at < offset + len; at += d->erase_size) {
		volatile u8 *blk = d->base + at;

		blk[0] = CFI_CMD_CLEAR_SR;
		blk[0] = CFI_CMD_ERASE;
		blk[0] = CFI_CMD_CONFIRM;
		if (mtd_wait(d->base) != 0)
			return -1;
	}
	return 0;
}

int mtd_write(struct mtd_device *d, u32 offset, const void *buf, u32 len)
{
	if (!d || !d->present || offset > d->size || len > d->size - offset)
		return -1;

	const u8 *in = buf;
	for (u32 i = 0; i < len; i++) {
		volatile u8 *at = d->base + offset + i;

		at[0] = CFI_CMD_CLEAR_SR;
		at[0] = CFI_CMD_PROGRAM;
		at[0] = in[i];
		if (mtd_wait(d->base) != 0)
			return -1;
	}
	return 0;
}

struct mtd_device *mtd_device_at(unsigned index)
{
	if (index >= MTD_MAX_DEVICES || !g_mtd[index].present)
		return 0;
	return &g_mtd[index];
}

/* ── the block face: /dev/mtdblock0 ──────────────────────────────────────
 *
 * Reading is direct. Writing goes read-modify-erase-write over the whole erase
 * block, because a block device caller expects to change 512 bytes without
 * knowing that the chip can only clear bits. That is slow and it is what the
 * hardware costs; the honest alternative would be to refuse writes entirely.
 */
static struct block_device g_mtd_blk[MTD_MAX_DEVICES];

static int mtdblock_read(struct block_device *dev, u64 lba, u32 count,
			 void *buffer)
{
	struct mtd_device *d = (struct mtd_device *)dev->priv;

	return mtd_read(d, (u32)(lba * dev->block_size),
			buffer, count * (u32)dev->block_size) == 0
		       ? (int)count
		       : -1;
}

static int mtdblock_write(struct block_device *dev, u64 lba, u32 count,
			  const void *buffer)
{
	struct mtd_device *d = (struct mtd_device *)dev->priv;
	u32 off = (u32)(lba * dev->block_size);
	u32 len = count * (u32)dev->block_size;
	const u8 *src = buffer;

	while (len > 0) {
		u32 block = off / d->erase_size * d->erase_size;
		u32 in_block = off - block;
		u32 chunk = d->erase_size - in_block;

		if (chunk > len)
			chunk = len;

		u8 *shadow = kmalloc(d->erase_size);
		if (!shadow)
			return -1;
		if (mtd_read(d, block, shadow, d->erase_size) != 0 ||
		    mtd_erase(d, block, d->erase_size) != 0) {
			kfree(shadow);
			return -1;
		}
		memcpy(shadow + in_block, src, chunk);
		if (mtd_write(d, block, shadow, d->erase_size) != 0) {
			kfree(shadow);
			return -1;
		}
		kfree(shadow);

		off += chunk;
		src += chunk;
		len -= chunk;
	}
	return (int)count;
}


/* Which chip a node names. The node is /dev/mtdN, so the digit is the index --
 * the inode carries no private pointer to hang a device on, and inventing a
 * side table keyed by node pointer would be a second place to get wrong. */
static struct mtd_device *mtd_from_node(struct vfs_node *node)
{
	const char *nm = node ? node->name : "";
	unsigned idx;

	if (nm[0] != 'm' || nm[1] != 't' || nm[2] != 'd')
		return 0;
	if (nm[3] < '0' || nm[3] > '9')
		return 0;
	idx = (unsigned)(nm[3] - '0');
	if (idx >= MTD_MAX_DEVICES || !g_mtd[idx].present)
		return 0;
	return &g_mtd[idx];
}

/* ── the character face: /dev/mtd0 ───────────────────────────────────────
 *
 * This is the interface that makes flash usable as flash: read and write are
 * plain, and MEMERASE is what a block device can never offer. flash_eraseall
 * and flashcp speak exactly these three.
 */
static isize mtd_dev_read(struct vfs_node *node, u64 offset, char *buffer,
			  usize size, int flags)
{
	struct mtd_device *d = mtd_from_node(node);

	(void)flags;
	if (!d || !d->present)
		return -ENODEV;
	if (offset >= d->size)
		return 0;
	if (size > d->size - offset)
		size = d->size - (u32)offset;
	if (mtd_read(d, (u32)offset, buffer, (u32)size) != 0)
		return -EIO;
	return (isize)size;
}

static isize mtd_dev_write(struct vfs_node *node, u64 offset,
			   const char *buffer, usize size, int flags)
{
	struct mtd_device *d = mtd_from_node(node);

	(void)flags;
	if (!d || !d->present)
		return -ENODEV;
	if (offset >= d->size)
		return -ENOSPC;
	if (size > d->size - offset)
		size = d->size - (u32)offset;
	/* No erase here: writing to flash without erasing first is what the
	 * hardware does, bits going one way only, and a caller using MTD is
	 * expected to have erased. Hiding that would make /dev/mtd0 a slow
	 * block device rather than a flash interface. */
	if (mtd_write(d, (u32)offset, buffer, (u32)size) != 0)
		return -EIO;
	return (isize)size;
}

static int mtd_dev_ioctl(struct vfs_node *node, u64 request, void *arg)
{
	struct mtd_device *d = mtd_from_node(node);

	if (!d || !d->present)
		return -ENODEV;

	switch (request & 0xFFFF) {
	case 0x4d01: { /* MEMGETINFO */
		struct mtd_info_user info;

		memset(&info, 0, sizeof(info));
		info.type = MTD_TYPE_NORFLASH;
		info.flags = MTD_CAP_NORFLASH;
		info.size = d->size;
		info.erasesize = d->erase_size;
		info.writesize = 1; /* NOR programs a byte at a time */
		info.oobsize = 0;   /* out-of-band data is a NAND thing */
		return syscall_copyout(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
	}
	case 0x4d02: { /* MEMERASE */
		struct erase_info_user er;

		if (syscall_copyin(&er, arg, sizeof(er)) < 0)
			return -EFAULT;
		return mtd_erase(d, er.start, er.length) == 0 ? 0 : -EIO;
	}
	case 0x4d0b: /* MEMGETBADBLOCK */
	case 0x4d0c: /* MEMSETBADBLOCK */
		/* Bad blocks are a NAND idea: NAND ships with factory-marked bad
		 * blocks and grows more, which is why its tools ask before every
		 * erase. NOR has none, so the honest answer is "not supported"
		 * rather than "unknown command" -- flash_eraseall stops outright
		 * on ENOTTY, and rightly, since a tool that cannot tell whether a
		 * block is bad must not erase blindly. */
		return -EOPNOTSUPP;
	case 0x4d03: /* MEMWRITEOOB */
	case 0x4d04: /* MEMREADOOB */
		/* NOR flash has no out-of-band area. Reporting "not supported"
		 * is the truth; returning zeroes would let a NAND tool believe
		 * it had read spare data that does not exist. */
		return -EOPNOTSUPP;
	case 0x4d07: { /* MEMGETREGIONCOUNT */
		int count = 1; /* uniform erase blocks: one region */

		return syscall_copyout(arg, &count, sizeof(count)) < 0 ? -EFAULT : 0;
	}
	default:
		return -ENOTTY;
	}
}

static void mtd_publish_char(unsigned index);

void mtd_create_dev_nodes(void)
{
	for (unsigned i = 0; i < MTD_MAX_DEVICES; i++)
		if (g_mtd[i].present)
			mtd_publish_char(i);
}

static void mtd_publish_char(unsigned index)
{
	char path[16];

	path[0] = '/'; path[1] = 'd'; path[2] = 'e'; path[3] = 'v';
	path[4] = '/'; path[5] = 'm'; path[6] = 't'; path[7] = 'd';
	path[8] = (char)('0' + index);
	path[9] = '\0';

	struct vfs_node *node = vfs_add_node(path, VFS_DEVICE, 0, 0, 0);

	if (!node || IS_ERR(node)) {
		klog_warn("mtd: could not publish the character device");
		return;
	}
	node->inode->mode = 0600;
	node->inode->read_cb = mtd_dev_read;
	node->inode->write_cb = mtd_dev_write;
	node->inode->ioctl_cb = mtd_dev_ioctl;
	vfs_node_put(node);
}

void mtd_init(void)
{
	if (!bootinfo_has_flag("b1nix.mtd"))
		return;

	/* Where to look. QEMU stacks pflash units downwards from 4 GiB with the
	 * firmware on top, so the second unit begins one firmware-size below the
	 * end of memory. The firmware is 256 KiB (SeaBIOS) or 4 MiB (OVMF), and
	 * the chip below it is whatever size it is -- hence a short list of
	 * candidates rather than one constant. Each is asked, not assumed. */
	static const u64 tops[] = { 0x100000000ull - 0x40000ull,
				    0x100000000ull - 0x400000ull };
	static const u32 sizes[] = { 0x400000, 0x800000, 0x1000000, 0x2000000,
				     0x100000, 0x200000 };
	unsigned found = 0;

	for (unsigned t = 0; t < sizeof(tops) / sizeof(tops[0]) &&
			     found < MTD_MAX_DEVICES; t++) {
		for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]) &&
				     found < MTD_MAX_DEVICES; s++) {
			u64 phys = tops[t] - sizes[s];

			if (mtd_probe_at(phys, &g_mtd[found]) != 0)
				continue;

			char *nm = kmalloc(12);
			if (!nm)
				return;
			nm[0] = 'm'; nm[1] = 't'; nm[2] = 'd';
			nm[3] = 'b'; nm[4] = 'l'; nm[5] = 'o';
			nm[6] = 'c'; nm[7] = 'k';
			nm[8] = (char)('0' + found);
			nm[9] = '\0';

			g_mtd[found].name = "mtd0";
			g_mtd_blk[found].name = nm;
			g_mtd_blk[found].bus = BLK_BUS_MTD;
			g_mtd_blk[found].block_size = 512;
			g_mtd_blk[found].block_count = g_mtd[found].size / 512;
			g_mtd_blk[found].priv = &g_mtd[found];
			g_mtd_blk[found].read_blocks = mtdblock_read;
			g_mtd_blk[found].write_blocks = mtdblock_write;
			blk_register(&g_mtd_blk[found]);

			mtd_publish_char(found);
			console_write("mtd: CFI NOR flash, ");
			console_write_dec(g_mtd[found].size / 1024);
			console_write(" KiB, erase block ");
			console_write_dec(g_mtd[found].erase_size / 1024);
			console_write(" KiB\n");
			found++;
			/* One chip per top: the next size would be the same
			 * chip seen at a different base. */
			break;
		}
	}

	if (!found)
		klog_warn("mtd: b1nix.mtd asked for, but no CFI chip answered");
}
