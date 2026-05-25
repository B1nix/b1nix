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

#define BLK_CACHE_DIRTY 0x01
#define BLK_CACHE_VALID 0x02

struct block_buffer {
    u64 block_no;
    u32 flags;
    u8 data[512]; // Assuming 512-byte blocks
    struct block_device *bdev;
    u32 last_used;
};

void blk_register(struct block_device *dev);
struct block_device *blk_get(const char *name);
usize blk_count(void);
struct block_device *blk_at(usize index);

// Block Cache API
void blk_cache_init(void);
int blk_read_cached(struct block_device *dev, u64 lba, u32 count, void *buffer);
int blk_write_cached(struct block_device *dev, u64 lba, u32 count, const void *buffer);
void blk_cache_flush(struct block_device *dev);
void blk_flush_buffer(struct block_buffer *buf);
void blk_sync_all(void);
void blk_cache_invalidate(struct block_device *dev);
int blk_cache_lock_is_held(void);

#endif
