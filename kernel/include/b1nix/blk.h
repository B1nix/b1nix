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
	BLK_BUS_MD,      /* software RAID — an array over other block devices */
	BLK_BUS_NBD,     /* a remote export reached over TCP */
	BLK_BUS_MTD,     /* raw flash, through the MTD layer */
};

/* What one command to this device may carry. Linux keeps the same three
 * numbers per request queue in struct queue_limits and every layer above the
 * driver clamps against them instead of against a constant of its own, which is
 * how one kernel drives a 32-slot AHCI port and a 1024-entry NVMe queue without
 * a compile-time compromise between the two. A driver fills these in before
 * blk_register(); whatever it leaves at zero is filled with the defaults below,
 * so a device that reports nothing still behaves exactly as it did. */
struct blk_queue_limits {
	/* Largest single transfer, in 512-byte sectors. */
	u32 max_sectors;
	/* Scatter-gather entries one command may reference. */
	u32 max_segments;
	/* Commands the driver keeps in flight at once. Every b1nix storage
	 * driver serialises on its own busy flag today, so this is 1 unless a
	 * driver says otherwise; it exists so the layers above can stop
	 * assuming it. */
	u32 queue_depth;
};

/* Defaults for a device that reports nothing. 2560 sectors is Linux's
 * BLK_DEF_MAX_SECTORS_CAP (1.25 MiB), 128 segments is its BLK_MAX_SEGMENTS,
 * and a depth of 1 is what b1nix's drivers actually deliver. */
#define BLK_DEF_MAX_SECTORS   2560u
#define BLK_DEF_MAX_SEGMENTS  128u
#define BLK_DEF_QUEUE_DEPTH   1u

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
	/* Per-device I/O limits. Filled in by the driver where the hardware
	 * reports them, normalised to the defaults by blk_register(). */
	struct blk_queue_limits limits;
	/* Optional (M109): tell the device that a range of blocks no longer holds
	 * anything worth keeping — virtio-blk DISCARD, NVMe DSM Deallocate, ATA
	 * DATA SET MANAGEMENT/TRIM. Only set by a driver whose device actually
	 * offered the command. Never call this directly: go through
	 * blk_discard_blocks(), which drops the stale cached copies first. */
	int (*discard)(struct block_device *dev, u64 lba, u32 count);
	/* Whether the medium is rotating, as BLKROTATIONAL reports it. Set by the
	 * driver from what the device says (ATA IDENTIFY word 217); every
	 * non-ATA transport here is solid-state or memory, so the default 0 is
	 * already the truth for them. */
	u8 rotational;
	/* Read-ahead state, per device. `ra_next` is the sector the last
	 * read-ahead ended at and `ra_run` the window it used, both updated
	 * without a lock: a wrong guess costs one badly sized read, never
	 * correctness. See blk_readahead_for(). */
	u64 ra_next;
	u32 ra_run;
	void *priv;
};

/* The device's own per-command sector ceiling, never zero. Use this instead of
 * a private constant anywhere a caller chops a transfer into driver-sized
 * pieces. A partition answers with its parent disk's limit. */
u32 blk_max_sectors(struct block_device *dev);
/* How many filesystem blocks of `block_size` may be folded into one device
 * request — derived from what a single command to `dev` can carry, with a floor
 * and a ceiling, and overridable with `b1nix.fs-run-max=N`. See blk.c. */
usize blk_run_blocks(struct block_device *dev, u32 block_size);
/* Read-ahead window in sectors for this device: the system-wide default
 * (RAM-independent, `b1nix.read-ahead-kb` on the command line) clamped to what
 * one command to this device can actually carry. */
u32 blk_readahead_sectors(struct block_device *dev);
/* The window to use for a miss at `lba`, in sectors.
 *
 * A fixed window is wrong in both directions: too small and a file read costs
 * a command per few kilobytes, too large and every random touch drags a
 * quarter of a megabyte behind it. Measured on a cold browser start, the flat
 * 256 KiB window read 520 MB off the disk for a working set a third that size.
 * This grows the window while reads continue where the last one ended and
 * collapses it the moment they do not, between a floor and a ceiling derived
 * from the device and the machine rather than a constant. */
u32 blk_readahead_for(struct block_device *dev, u64 lba);

/* Record who the next dirtying writes belong to, on THIS cpu, for as long as
 * the filesystem is inside one file's write. Passing it down through every
 * block-layer call would mean changing every signature for a hint. */
void blk_set_dirty_owner(u32 fsid, u64 ino);
void blk_clear_dirty_owner(void);
/* Write back only the dirty blocks stamped with this owner (plus unstamped
 * ones), then flush the device. */
int blk_cache_flush_inode(struct block_device *dev, u32 fsid, u64 ino);

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
    /* Who dirtied this block. fsync has to write back one file's blocks, not
     * every dirty block on the device; without an owner it had to flush the
     * whole cache, which is why one fdatasync cost seconds. Set from the
     * filesystem's write path (see blk_set_dirty_owner); zero means "no owner
     * recorded", and those are flushed by any fsync, conservatively. */
    u32 dirty_fsid;
    u64 dirty_ino;
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

/* The major number b1nix publishes every block device under. One number for
 * the whole class, with the registry index as the minor — the same pair
 * /proc/partitions, /proc/self/mountinfo and /sys/dev/block already print, so
 * a node userspace mknod()s from what /sys told it resolves back to the right
 * device. */
#define BLK_SYSFS_MAJOR 8

/* Remove a device from the registry (LOOP_CTL_REMOVE today). Raises the
 * `remove` uevent, empties the slot WITHOUT renumbering anything above it, and
 * leaves /dev alone — the node is userspace's to remove, which is what mdev
 * does when it reads the event. 0, or -ENODEV if it was never registered. */
int blk_unregister(struct block_device *dev);

/* Bumped on every register and unregister. A cached view of the registry
 * (/sys/block and its mirrors) compares this against the value it was built
 * from instead of rebuilding on every readdir. */
u32 blk_generation(void);

/* Tell sysfs the registry changed (implemented in kernel/fs/sysfs/sysfs.c). The
 * block layer has to push: the path resolver only calls a directory's
 * lookup_cb when the name MISSES, so a stale /sys/block/<device> would keep
 * resolving and no readdir-time refresh could ever retire it. No-op before
 * /sys is mounted. */
void sysfs_block_changed(void);

/* The device a (major << 8 | minor) pair names, or NULL. Only BLK_SYSFS_MAJOR
 * is a block major here. */
struct block_device *blk_from_devno(u64 rdev);

/* Make `node` behave as the block device `rdev` names: cached block I/O
 * through read/write, the device's size, and st_rdev. This is what mknod(2)
 * calls for an S_IFBLK node, so a node created by mdev from a hot-plug event
 * works exactly like one the kernel created at boot. */
struct vfs_node;
int blk_bind_dev_node(struct vfs_node *node, u64 rdev);

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
/* Volume identity, read from the on-disk superblock: the canonical
 * 8-4-4-4-12 UUID (ext) or XXXX-XXXX serial (FAT/exFAT), and the volume
 * label. 0 on success, -1 when the filesystem carries no such field.
 * `out` needs 37 bytes for a UUID. */
int blk_probe_uuid(struct block_device *dev, char *out, usize cap);
int blk_probe_label(struct block_device *dev, char *out, usize cap);

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
/* The writeback deadline a filesystem should defer metadata against. */
u64 blk_writeback_interval_ticks(void);
/* Make `count` blocks starting at `lba` read back as zeroes. Uses the device's
 * own WRITE ZEROES when it has one (keeping the block cache coherent), and
 * otherwise writes the zeroes through the cache exactly as before. */
int blk_zero_blocks(struct block_device *dev, u64 lba, u32 count);
/* Discard `count` blocks starting at `lba` (BLKDISCARD, and the FITRIM walk).
 * Returns 0, -EOPNOTSUPP when the device offers no discard command, or -EIO.
 * There is deliberately no software fallback — see the comment on the
 * definition. */
int blk_discard_blocks(struct block_device *dev, u64 lba, u32 count);
/* Whether blk_discard_blocks() would do anything for this device. */
int blk_discard_supported(struct block_device *dev);
void blk_cache_flush(struct block_device *dev);
void blk_flush_buffer(struct block_buffer *buf);
void blk_sync_all(void);
void blk_cache_invalidate(struct block_device *dev);
/* `b1nix.blk-torture`: rewrite one multi-sector range repeatedly and check the
 * medium keeps up. Test mode only; silent unless asked for. */
void blk_cache_torture_test(void);
/* Forget a range, without writing it back — for a caller that has already put
 * the authoritative bytes on the medium. */
void blk_cache_invalidate_range(struct block_device *dev, u64 lba, u32 count);
int blk_cache_lock_is_held(void);

/* M14 self-test (b1nix.test=1 only): every disk that claims a cache-flush
 * command accepts one, and on a scratch virtio disk a pattern survives
 * flush + cache-drop and blk_zero_blocks() really zeroes. */
void blk_durability_selftest(void);

/* M107 loop devices: (re)register /dev/loop-control after a root mount. */
void loop_register_nodes(void);

#endif
