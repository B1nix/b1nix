#ifndef B1NIX_BLK_H
#define B1NIX_BLK_H

#include <b1nix/types.h>

struct block_device {
	const char *name;
	usize block_size;
	u64 block_count;
	int (*read_blocks)(struct block_device *dev, u64 lba, u32 count, void *buffer);
	int (*write_blocks)(struct block_device *dev, u64 lba, u32 count, const void *buffer);
	void *priv;
};

void blk_register(struct block_device *dev);
struct block_device *blk_get(const char *name);

#endif
