#include <b1nix/blk.h>
#include <b1nix/mm.h>
#include <string.h>
#include <stdbool.h>

#define MAX_BLK_DEVICES 8
#define CACHE_ENTRIES 256
#define CACHE_BLOCK_SIZE 512

static struct block_device *blk_devices[MAX_BLK_DEVICES];
static usize blk_device_count = 0;

struct blk_cache_entry {
	struct block_device *dev;
	u64 lba;
	u8 data[CACHE_BLOCK_SIZE];
	u32 last_used;
	bool dirty;
	bool valid;
};

static struct blk_cache_entry bcache[CACHE_ENTRIES];
static u32 bcache_tick = 0;

void blk_register(struct block_device *dev)
{
	if (blk_device_count < MAX_BLK_DEVICES) {
		blk_devices[blk_device_count++] = dev;
	}
}

struct block_device *blk_get(const char *name)
{
	for (usize i = 0; i < blk_device_count; i++) {
		if (strcmp(blk_devices[i]->name, name) == 0) {
			return blk_devices[i];
		}
	}
	return 0;
}

void blk_cache_init(void)
{
	memset(bcache, 0, sizeof(bcache));
}

static struct blk_cache_entry *bcache_find(struct block_device *dev, u64 lba)
{
	for (int i = 0; i < CACHE_ENTRIES; i++) {
		if (bcache[i].valid && bcache[i].dev == dev && bcache[i].lba == lba) {
			bcache[i].last_used = ++bcache_tick;
			return &bcache[i];
		}
	}
	return 0;
}

static struct blk_cache_entry *bcache_evict(void)
{
	int oldest_idx = 0;
	u32 oldest_tick = 0xFFFFFFFF;
	
	for (int i = 0; i < CACHE_ENTRIES; i++) {
		if (!bcache[i].valid) {
			return &bcache[i];
		}
	}
	
	for (int i = 0; i < CACHE_ENTRIES; i++) {
		if (bcache[i].last_used < oldest_tick) {
			oldest_tick = bcache[i].last_used;
			oldest_idx = i;
		}
	}
	
	struct blk_cache_entry *entry = &bcache[oldest_idx];
	if (entry->dirty && entry->dev && entry->dev->write_blocks) {
		entry->dev->write_blocks(entry->dev, entry->lba, 1, entry->data);
		entry->dirty = false;
	}
	entry->valid = false;
	return entry;
}

int blk_read_cached(struct block_device *dev, u64 lba, u32 count, void *buffer)
{
	if (!dev || !dev->read_blocks) return -1;
	if (dev->block_size != CACHE_BLOCK_SIZE) {
		return dev->read_blocks(dev, lba, count, buffer);
	}
	
	u8 *buf8 = (u8 *)buffer;
	for (u32 i = 0; i < count; i++) {
		u64 current_lba = lba + i;
		struct blk_cache_entry *entry = bcache_find(dev, current_lba);
		
		if (entry) {
			memcpy(buf8 + i * CACHE_BLOCK_SIZE, entry->data, CACHE_BLOCK_SIZE);
		} else {
			entry = bcache_evict();
			entry->dev = dev;
			entry->lba = current_lba;
			if (dev->read_blocks(dev, current_lba, 1, entry->data) < 0) {
				return -1;
			}
			entry->valid = true;
			entry->dirty = false;
			entry->last_used = ++bcache_tick;
			memcpy(buf8 + i * CACHE_BLOCK_SIZE, entry->data, CACHE_BLOCK_SIZE);
		}
	}
	return 0;
}

int blk_write_cached(struct block_device *dev, u64 lba, u32 count, const void *buffer)
{
	if (!dev || !dev->write_blocks) return -1;
	if (dev->block_size != CACHE_BLOCK_SIZE) {
		return dev->write_blocks(dev, lba, count, buffer);
	}
	
	const u8 *buf8 = (const u8 *)buffer;
	for (u32 i = 0; i < count; i++) {
		u64 current_lba = lba + i;
		struct blk_cache_entry *entry = bcache_find(dev, current_lba);
		
		if (!entry) {
			entry = bcache_evict();
			entry->dev = dev;
			entry->lba = current_lba;
			entry->valid = true;
			entry->last_used = ++bcache_tick;
		}
		
		memcpy(entry->data, buf8 + i * CACHE_BLOCK_SIZE, CACHE_BLOCK_SIZE);
		entry->dirty = true;
		
		// Write-through for safety right now
		if (dev->write_blocks(dev, current_lba, 1, entry->data) == 0) {
			entry->dirty = false;
		}
	}
	return 0;
}

void blk_cache_flush(struct block_device *dev)
{
	if (!dev || !dev->write_blocks) return;
	for (int i = 0; i < CACHE_ENTRIES; i++) {
		if (bcache[i].valid && bcache[i].dev == dev && bcache[i].dirty) {
			dev->write_blocks(dev, bcache[i].lba, 1, bcache[i].data);
			bcache[i].dirty = false;
		}
	}
}
