#ifndef B1NIX_BLK_H
#define B1NIX_BLK_H

#include <b1nix/types.h>

/* What kind of transport a block device sits on. Callers that need to pick a
 * device by what it *is* — the live-ISO boot medium, the dedicated swap disk —
 * ask for a bus, never for a name prefix: a name is a label the block layer
 * hands out, and USB storage answers to sdb or sdc depending only on the order
 * the buses probed. */
enum blk_bus {
	BLK_BUS_UNKNOWN = 0,
	BLK_BUS_ATA,     /* SATA/AHCI — named sd* */
	BLK_BUS_USB,     /* USB mass storage — also SCSI, so also named sd* */
	BLK_BUS_NVME,
	BLK_BUS_VIRTIO,
	BLK_BUS_MEMORY,  /* ramdisk */
	BLK_BUS_LOOP,
};

struct block_device {
	const char *name;
	/* enum blk_bus. A partition inherits its parent's bus. */
	u8 bus;
	usize block_size;
	u64 block_count;
	int (*read_blocks)(struct block_device *dev, u64 lba, u32 count, void *buffer);
	int (*write_blocks)(struct block_device *dev, u64 lba, u32 count, const void *buffer);
	/* Optional: push whatever sits below this device out to stable storage once
	 * the block cache has been drained into it. A loop device's write_blocks
	 * only reaches the backing file's page cache, so fsync() on /dev/loopN has
	 * to continue down that second level. Real disks leave this NULL. */
	int (*flush)(struct block_device *dev);
	/* Optional: make a range of blocks read back as zeroes without the caller
	 * having to DMA a zero-filled buffer at the device. Only set by a driver
	 * whose device negotiated the capability (virtio-blk's WRITE ZEROES today).
	 * Never call this directly — it writes behind the block cache's back; go
	 * through blk_zero_blocks(), which drops the stale cached copies first. */
	int (*write_zeroes)(struct block_device *dev, u64 lba, u32 count);
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

/* Device naming. b1nix uses the names the rest of Unix uses; the suffix comes
 * from the position in the sequence, so no driver carries a table.
 *   blk_disk_name("sd", 0/1/4, ...)  -> "sda" / "sdb" / "sde"
 *   blk_disk_name("vd", 0, ...)      -> "vda"
 *   blk_nvme_name(0, 1, ...)         -> "nvme0n1"
 * Partitions are named by the block layer itself: sda1, vda1, nvme0n1p1. */
void blk_disk_name(const char *prefix, usize index, char *out, usize out_size);
void blk_nvme_name(usize controller, u32 nsid, char *out, usize out_size);

/* Register a whole disk under the next free name in `prefix`'s sequence and
 * record which bus it came from. The sequence is owned here, not by the driver,
 * so every SCSI-class disk — AHCI and USB mass storage alike — draws from ONE
 * "sd" sequence and the fifth disk is sde whichever bus delivered it. The
 * driver fills in sizes and callbacks first; this assigns ->name (kmalloc'd),
 * ->bus, and registers. Registration order decides the letter, as on Linux. */
void blk_register_disk(struct block_device *dev, const char *prefix, u8 bus);

void blk_register(struct block_device *dev);

/* The nth (0-based) whole disk on a given bus, or NULL. Lets a caller say
 * "the second ATA disk" without spelling a name that only happens to match. */
struct block_device *blk_nth_on_bus(u8 bus, usize n);
/* Removable medium (USB mass storage today) — what /sys/block/<d>/removable
 * reports, and how the live-ISO boot path finds candidate media. */
int blk_is_removable(struct block_device *dev);
struct block_device *blk_get(const char *name);
usize blk_count(void);
struct block_device *blk_at(usize index);
/* dev_t (major << 8 | minor) of a registered block device — what st_dev and
 * /proc/<pid>/maps report for a filesystem mounted from it. 0 = not registered. */
u32 blk_devno(struct block_device *dev);
const char *blk_probe_fstype(struct block_device *dev);

/* Partition introspection (backs sysfs /sys/block). A registered device is a
 * partition if it was created by the MBR/GPT scanner; otherwise it is a whole
 * disk. blk_partition_number reads the trailing number of the name (1-based). */
int blk_is_partition(struct block_device *dev);
struct block_device *blk_partition_parent(struct block_device *dev);
int blk_partition_number(struct block_device *dev);
int blk_rescan_partitions(struct block_device *dev);

/* Create a /dev/<name> node for every registered block device (read/write
 * translated to cached block I/O + BLK* size ioctls). Call once after all
 * storage drivers have probed. Backs BusyBox blkid/fdisk and /proc/partitions. */
void blk_create_dev_nodes(void);

// Block Cache API
void blk_cache_init(void);
int blk_read_cached(struct block_device *dev, u64 lba, u32 count, void *buffer);
int blk_write_cached(struct block_device *dev, u64 lba, u32 count, const void *buffer);
/* Make `count` blocks starting at `lba` read back as zeroes. Uses the device's
 * own WRITE ZEROES when it has one (keeping the block cache coherent), and
 * otherwise writes the zeroes through the cache exactly as before. */
int blk_zero_blocks(struct block_device *dev, u64 lba, u32 count);
void blk_cache_flush(struct block_device *dev);
void blk_flush_buffer(struct block_buffer *buf);
void blk_sync_all(void);
void blk_cache_invalidate(struct block_device *dev);
int blk_cache_lock_is_held(void);

/* M14 self-test (b1nix.test=1 only): every disk that claims a cache-flush
 * command accepts one, and on a scratch virtio disk a pattern survives
 * flush + cache-drop and blk_zero_blocks() really zeroes. */
void blk_durability_selftest(void);

/* M107 loop devices: (re)register /dev/loop-control after a root mount. */
void loop_register_nodes(void);

#endif
