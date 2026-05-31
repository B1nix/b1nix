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
/* In-flight: a CPU has claimed this entry and is doing its (lock-free, yielding)
 * block DMA into ->data. Eviction must skip BUSY entries, otherwise a second CPU
 * could pick the same slot and DMA a different block into it — corrupting both
 * (observed under -smp4 parallel builds as gcc reading garbage from a header). */
#define BLK_CACHE_BUSY  0x04

struct block_buffer {
    u64 block_no;
    u32 flags;
    u8 data[512]; // Assuming 512-byte blocks
    struct block_device *bdev;
    u32 last_used;
    /* Singly-linked hash-chain index in the bcache, -1 = end of chain.
     * Lets bcache_find lookup an (bdev, block_no) pair without scanning the
     * full block_cache[] array. block_cache scales with RAM (~2 entries per
     * MiB), so the old linear scan made gcc execve O(cache_size) per binary
     * page read — at 4 GiB guests that was 8K comparisons per file page,
     * dwarfing everything else. */
    i32 hash_next;
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
