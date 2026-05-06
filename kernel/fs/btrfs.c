#include <b1nix/blk.h>
#include <b1nix/btrfs.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

#define BTRFS_SUPER_OFFSET 65536ULL
#define BTRFS_SUPER_SIZE 4096
#define BTRFS_MAGIC_OFFSET 0x40
#define BTRFS_LABEL_OFFSET 0x12b
#define BTRFS_LABEL_SIZE 256

static const char btrfs_magic[8] = {'_', 'B', 'H', 'R', 'f', 'S', '_', 'M'};

static int btrfs_read_super(struct block_device *dev, u8 *buffer)
{
	if (!dev || dev->block_size == 0) return -1;
	u64 lba = BTRFS_SUPER_OFFSET / dev->block_size;
	u32 count = BTRFS_SUPER_SIZE / dev->block_size;
	if (count == 0) count = 1;
	return blk_read_cached(dev, lba, count, buffer);
}

static int btrfs_probe(struct block_device *dev, char *label, usize label_size)
{
	u8 *super = kmalloc(BTRFS_SUPER_SIZE);
	if (!super) return -1;

	int ok = btrfs_read_super(dev, super) == 0 &&
	         memcmp(super + BTRFS_MAGIC_OFFSET, btrfs_magic, sizeof(btrfs_magic)) == 0;
	if (ok && label && label_size > 0) {
		usize len = 0;
		while (len < BTRFS_LABEL_SIZE && len < label_size - 1 &&
		       super[BTRFS_LABEL_OFFSET + len] != 0) {
			label[len] = (char)super[BTRFS_LABEL_OFFSET + len];
			len++;
		}
		label[len] = '\0';
	}

	kfree(super);
	return ok ? 0 : -1;
}

int btrfs_mount_root(const char *device_name, const char *mount_point)
{
	struct block_device *dev = blk_get(device_name);
	if (!dev) return -1;

	char label[64];
	label[0] = '\0';
	if (btrfs_probe(dev, label, sizeof(label)) != 0) {
		return -1;
	}

	vfs_mount(device_name, mount_point, "btrfs", 0);
	vfs_add_node(mount_point, VFS_DIRECTORY, 0, 0, 0);

	console_write("btrfs: mounted ");
	console_write(device_name);
	console_write(" at ");
	console_write(mount_point);
	if (label[0]) {
		console_write(" label ");
		console_write(label);
	}
	console_write(" (metadata-only)\n");
	return 0;
}

void btrfs_init(void)
{
	for (usize i = 0; i < blk_count(); i++) {
		struct block_device *dev = blk_at(i);
		if (!dev) continue;
		if (btrfs_probe(dev, 0, 0) == 0) {
			console_write("btrfs: found filesystem on ");
			console_write(dev->name);
			console_write("\n");
		}
	}
}
