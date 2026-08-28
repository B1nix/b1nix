#include <b1nix/kprintf.h>
#include <b1nix/lapic.h>
#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/acpi.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
#include <b1nix/uevent.h>
#include <b1nix/errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_BLK_DEVICES 64
#define MAX_BLK_PARTITIONS 64
/* Block-cache sizing (B2 audit): pool capacity is now scaled to actual RAM
 * at blk_cache_init() time so small machines don't waste ~140 KB on a 256-
 * entry pool and big machines aren't starved. ~1 entry per 512 KiB of usable
 * RAM, clamped to [MIN, MAX]. */
#define CACHE_ENTRIES_MIN 64
/* The ceiling, not the scaling rule, is what decided this pool's size.
 *
 * Entries scale with RAM, but 8192 of them is 4 MiB of cache — on an 8 GiB
 * machine, 0.05% of memory — and everything above that ceiling went to the
 * disk every time. Measured on a browser start: 20000 requests, 96% of which
 * had to wait, 34 seconds spent waiting. A working set of a few hundred MiB
 * (one Chromium binary is 220 MiB) cannot even begin to stay resident.
 *
 * 65536 entries is 32 MiB — still well under 1% of an 8 GiB guest, and the
 * per-RAM rule below keeps small guests small: it reaches this ceiling only at
 * 8 GiB, and a 512 MiB self-host guest still gets the same 4 MiB it had. */
#define CACHE_ENTRIES_MAX 262144
#define CACHE_BLOCK_SIZE 512


/* Read-ahead window (sectors) pulled in ONE device command on a cache miss.
 * The cache fills one 512-byte sector per dev->read_blocks() call; streaming a
 * large file (an executable, a toolchain binary) that way costs one DMA round-
 * trip per sector — ~184k commands for a 94 MB binary, the dominant cost of a
 * disk-resident toolchain. Reading a contiguous run in a single command and
 * populating the neighbouring cache slots from the data already in hand turns
 * a sequential stream into ~1 command per RA sectors. Under KVM the per-command
 * cost is the polled-completion VM-exits (roughly constant), so a bigger window
 * means fewer commands and higher throughput.
 *
 * The window is a system-wide default in kilobytes, exactly as Linux expresses
 * read_ahead_kb (its default is 128 KiB; ours is 256 KiB because the polled
 * completion path makes a command dearer here than it is there), clamped per
 * device to what one command to THAT device can carry — the AHCI PRDT, the
 * virtio descriptor chain and the NVMe PRP list all differ, and a constant that
 * fits the smallest of them wastes the others. `b1nix.read-ahead-kb=N`
 * overrides the default; 0 disables read-ahead entirely.
 *
 * The 256 KiB default is measured, not chosen: at 64 KiB a demand-paging fault
 * cost 17.6 ms and at 256 KiB it cost 0.157 ms — a hundredfold, on the same
 * kernel and the same workload. The reason is not the transfer but the request:
 * a virtio request that does not complete promptly stalls until its watchdog
 * re-poll, and a large read amortises that stall over five hundred sectors
 * instead of sixty-four. */
#define BCACHE_READAHEAD_KB_DEF 256u
#define BCACHE_READAHEAD_DEF (BCACHE_READAHEAD_KB_DEF * 1024u / CACHE_BLOCK_SIZE)
static u32 bcache_ra_sectors = BCACHE_READAHEAD_DEF;

/* Who the writes happening on this CPU belong to. The filesystem sets it
 * around a file's write so the block cache can record an owner for each block
 * it dirties, and fsync can then write back that file instead of the whole
 * device. Per-CPU because two CPUs write different files at the same time. */
#define BLK_OWNER_CPUS 64
static struct { u32 fsid; u64 ino; } blk_dirty_owner[BLK_OWNER_CPUS];

static unsigned blk_owner_slot(void) {
  struct percpu *pc = get_percpu();

  return pc ? (unsigned)(pc->cpu_id % BLK_OWNER_CPUS) : 0;
}

void blk_set_dirty_owner(u32 fsid, u64 ino) {
  unsigned s = blk_owner_slot();

  blk_dirty_owner[s].fsid = fsid;
  blk_dirty_owner[s].ino = ino;
}

void blk_clear_dirty_owner(void) {
  unsigned s = blk_owner_slot();

  blk_dirty_owner[s].fsid = 0;
  blk_dirty_owner[s].ino = 0;
}

static struct block_device *blk_devices[MAX_BLK_DEVICES];
static usize blk_device_count = 0;

/* Bumped every time a device joins or leaves the registry. Anything that
 * caches a view of the registry — /sys/block and its two mirrors — compares
 * this against what it built from and rebuilds only when it differs, so a
 * readdir of /sys costs one integer load in the common case where nothing has
 * been plugged in since. */
static volatile u32 blk_gen = 1;

u32 blk_generation(void) {
  return __atomic_load_n(&blk_gen, __ATOMIC_ACQUIRE);
}

struct partition_device {
  struct block_device blk;
  struct block_device *parent;
  u64 start_lba;
};

static struct partition_device partitions[MAX_BLK_PARTITIONS];
static usize partition_count;

static void blk_scan_partitions(struct block_device *dev);
/* Priority-gated device commands — see the I/O priority gate below. */
static int blk_dev_read(struct block_device *dev, u64 lba, u32 count, void *buf);
static int blk_dev_write(struct block_device *dev, u64 lba, u32 count,
                         const void *buf);

/* ── Block Cache Structures ── */

static struct block_buffer *block_cache = 0;
static usize block_cache_n = 0;
static u32 bcache_tick = 0;
static spinlock_t bcache_lock = SPINLOCK_INIT;

/* Hash table indexing block_cache[] by (bdev, block_no). Each bucket holds
 * an index into block_cache[] (or -1 for empty); collisions chain through
 * block_buffer.hash_next. Without this, bcache_find linearly scanned the
 * entire cache on every block read — at 4 GiB guests that's 8K comparisons
 * per cache lookup, and dominated gcc execve wall-clock (turning a 5 s
 * binary load into a 30 s one). Hash sized so average chain length stays
 * below ~8 even at CACHE_ENTRIES_MAX. */
/* Sized from the pool it actually indexes, not from the pool's ceiling. The
 * pool scales with RAM; a fixed 16384-bucket table was 64 KiB of BSS a 256 MiB
 * guest never needed and, had the ceiling ever risen, the one cache dimension
 * that would not have followed. Linux builds its dentry and inode hashes the
 * same way in alloc_large_system_hash(): a power of two derived from how many
 * objects there will be. One bucket per four entries keeps the average chain at
 * four, which is what the measurement above wanted. */
/* The ceiling is derived from the pool it indexes rather than written down: at
 * one bucket per four entries, CACHE_ENTRIES_MAX entries want
 * CACHE_ENTRIES_MAX/4 buckets, and a constant here would silently break that
 * invariant the next time the pool ceiling is raised — the table would stop
 * growing and the chains would lengthen instead, which is exactly the failure
 * this sizing exists to prevent. */
#define BCACHE_HASH_MIN_BITS 10
#define BCACHE_HASH_MAX_ENTRIES (CACHE_ENTRIES_MAX / 4u)
static i32 *bcache_hash;
static u32 bcache_hash_mask;

static inline u32 bcache_bucket(struct block_device *dev, u64 lba) {
  /* Spread bdev pointer bits across the lba — both are dense in low bits,
   * a plain XOR would collide for sequential reads from the same device. */
  u64 m = (u64)(usize)dev * 0x9E3779B97F4A7C15ULL;
  m ^= lba * 0xC6BC279692B5C323ULL;
  return (u32)((m ^ (m >> 32)) & bcache_hash_mask);
}

/* Caller must hold bcache_lock. Unlinks block_cache[idx] from its hash chain
 * (a no-op if not currently linked, i.e. invalid/uninitialized entry). */
static void bcache_hash_remove(i32 idx) {
  struct block_buffer *b = &block_cache[idx];
  if (!b->bdev) return; /* never inserted */
  u32 h = bcache_bucket(b->bdev, b->block_no);
  i32 *pp = &bcache_hash[h];
  while (*pp != -1) {
    if (*pp == idx) {
      *pp = b->hash_next;
      b->hash_next = -1;
      return;
    }
    pp = &block_cache[*pp].hash_next;
  }
  /* If we get here the chain was inconsistent — leave gracefully. */
  b->hash_next = -1;
}

/* Caller must hold bcache_lock. Links idx at the head of its hash bucket. */
static void bcache_hash_insert(i32 idx) {
  struct block_buffer *b = &block_cache[idx];
  u32 h = bcache_bucket(b->bdev, b->block_no);
  b->hash_next = bcache_hash[h];
  bcache_hash[h] = idx;
}

#include <b1nix/lockdep.h>

/* CPU that currently owns bcache_lock (-1 = unheld). The held check below must
 * be PER-CPU: its only caller (vfs_inode_lock_*) panics if *this* execution
 * context holds the block-cache lock while taking an inode lock (deadlock-order
 * guard). A plain spin_is_locked() is global — under SMP it reports "held" when
 * ANY core holds it, so a core doing an ordinary VFS lookup would falsely panic
 * merely because another core was concurrently in the block cache. That is why
 * -smp4 builds tripped `inode read-lock under block-cache lock` while -smp1
 * (single core, no concurrent holder) never did. The lock is taken IRQ-safe, so
 * the owning core can't be preempted mid-section and the field stays stable. */
static volatile int bcache_owner_cpu = -1;

static u64 bcache_acquire(void) {
  u64 flags;
  spin_lock_irqsave(&bcache_lock, &flags);
  struct percpu *p = get_percpu();
  bcache_owner_cpu = p ? (int)p->cpu_id : -1;
  LOCKDEP_ACQUIRE(LOCKDEP_LVL_BCACHE);
  return flags;
}

static void bcache_release(u64 flags) {
  LOCKDEP_RELEASE(LOCKDEP_LVL_BCACHE);
  bcache_owner_cpu = -1;
  spin_unlock_irqrestore(&bcache_lock, flags);
}

int blk_cache_lock_is_held(void) {
  struct percpu *p = get_percpu();
  if (!p)
    return 0; /* no per-CPU area => cannot be the holder */
  return bcache_owner_cpu == (int)p->cpu_id;
}

static u32 le32(const u8 *p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 le16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }

static u64 le64(const u8 *p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

/* ── Partition I/O ── */

static int partition_read(struct block_device *dev, u64 lba, u32 count,
                          void *buffer) {
  struct partition_device *part = (struct partition_device *)dev->priv;
  if (!part || count > dev->block_count || lba > dev->block_count - count)
    return -1;
  return blk_read_cached(part->parent, part->start_lba + lba, count, buffer);
}

static int partition_write(struct block_device *dev, u64 lba, u32 count,
                           const void *buffer) {
  struct partition_device *part = (struct partition_device *)dev->priv;
  if (!part || count > dev->block_count || lba > dev->block_count - count)
    return -1;
  return blk_write_cached(part->parent, part->start_lba + lba, count, buffer);
}

u32 blk_max_sectors(struct block_device *dev) {
  if (!dev || dev->limits.max_sectors == 0)
    return BLK_DEF_MAX_SECTORS;
  return dev->limits.max_sectors;
}

/* How many filesystem blocks of `block_size` a filesystem may fold into ONE
 * device request. Derived from the device, not compiled in.
 *
 * A filesystem that finds a run of adjacent blocks issues it as a single
 * blk_read_cached()/blk_write_cached() straight into the caller's buffer, so
 * the real limit is what one command to THIS device can carry: the AHCI PRDT,
 * the virtio descriptor chain and the NVMe PRP list all differ, and a constant
 * that fits the smallest of them wastes the others. A flat 64 blocks made a
 * 1 MiB read of a contiguous file four commands where one would have done.
 *
 * FLOOR   BLK_RUN_BLOCKS_MIN (16 blocks — 64 KiB at a 4 KiB block size). Below
 *         this the per-request cost the coalescing exists to amortise comes
 *         back, and a device reporting an implausibly small limit should not
 *         disable the optimisation altogether.
 * CEILING BLK_RUN_BLOCKS_MAX (512 blocks — 2 MiB at a 4 KiB block size).
 *         Nothing is allocated per run, so this bounds only the request size,
 *         and past 2 MiB one command stops getting cheaper per byte.
 * `b1nix.fs-run-max=N` states the run directly, in filesystem blocks. */
#define BLK_RUN_BLOCKS_MIN 16u
#define BLK_RUN_BLOCKS_MAX 512u

usize blk_run_blocks(struct block_device *dev, u32 block_size) {
  u32 spb = block_size / 512; /* 512-byte sectors per filesystem block */
  u32 blocks = spb ? blk_max_sectors(dev) / spb : 0;

  blocks = bootinfo_get_u32("b1nix.fs-run-max", blocks);
  if (blocks < BLK_RUN_BLOCKS_MIN)
    blocks = BLK_RUN_BLOCKS_MIN;
  if (blocks > BLK_RUN_BLOCKS_MAX)
    blocks = BLK_RUN_BLOCKS_MAX;
  return (usize)blocks;
}

/* Floor and ceiling for the adaptive window, both derived rather than fixed.
 *
 * The floor is what a single random touch is allowed to drag in — small enough
 * that a scattered access pattern pays almost nothing for guessing wrong. The
 * ceiling is the configured window (itself `b1nix.read-ahead-kb`) scaled by how
 * much memory there is to hold what it reads, and clamped to what one command
 * to this device can carry: read-ahead that cannot be cached is read twice. */
#define BCACHE_RA_MIN_SECTORS 16u /* 8 KiB */

static u32 blk_readahead_ceiling(struct block_device *dev) {
  u32 want = blk_readahead_sectors(dev);
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);

  /* A machine with room to spare reads further ahead; a small one would only
   * evict what it just read. The steps are deliberately coarse — this decides
   * an I/O size, not an allocation. */
  if (ram_mb >= 2048)
    want *= 4;
  else if (ram_mb >= 1024)
    want *= 2;
  else if (ram_mb < 256)
    want /= 2;

  u32 dev_max = blk_max_sectors(dev);
  if (want > dev_max)
    want = dev_max;
  if (want < BCACHE_RA_MIN_SECTORS)
    want = BCACHE_RA_MIN_SECTORS;
  return want;
}

u32 blk_readahead_for(struct block_device *dev, u64 lba) {
  if (!dev)
    return 1;
  u32 ceiling = blk_readahead_ceiling(dev);
  if (bcache_ra_sectors == 0)
    return 1; /* read-ahead switched off on the command line */

  u32 run = dev->ra_run;
  if (dev->ra_run && lba == dev->ra_next) {
    /* Continuing where the last window ended: this is a stream, so double it
     * until the ceiling. */
    run = run < ceiling / 2 ? run * 2 : ceiling;
  } else {
    /* A jump. Reading a quarter of a megabyte around a single random block is
     * how a cold start read half a gigabyte for a working set a third that
     * size, so start over from the floor and let a real stream earn the
     * window back. */
    run = BCACHE_RA_MIN_SECTORS;
  }
  if (run > ceiling)
    run = ceiling;
  dev->ra_run = run;
  dev->ra_next = lba + run;
  return run;
}

u32 blk_readahead_sectors(struct block_device *dev) {
  u32 want = bcache_ra_sectors;
  if (want == 0)
    return 1; /* read-ahead switched off on the command line */
  u32 dev_max = blk_max_sectors(dev);
  return want < dev_max ? want : dev_max;
}

/* The /sys path this device is published at, with the /sys prefix stripped —
 * what a uevent carries as DEVPATH and what mdev turns back into
 * /sys/block/<name> to read the device's `dev` and `uevent` files. */
static void blk_devpath(struct block_device *dev, char *out, usize cap) {
  struct block_device *parent = blk_partition_parent(dev);
  if (parent && parent->name)
    snprintf(out, cap, "/block/%s/%s", parent->name, dev->name);
  else
    snprintf(out, cap, "/block/%s", dev->name);
}

/* Announce a device appearing or leaving. Harmless with nothing listening, so
 * the drivers that register during early boot need no special case. */
static void blk_announce(struct block_device *dev, usize index,
                         const char *action) {
  if (!dev || !dev->name)
    return;
  char devpath[96];
  blk_devpath(dev, devpath, sizeof(devpath));
  /* DEVTYPE is what tells udev a disk from a partition, and it is the property
   * systemd-udevd looks for FIRST: a device whose message omits it is one
   * whose whole-disk question has no answer, and the worker drops the event
   * before a single rule runs. */
  uevent_post(action, devpath, "block",
              blk_is_partition(dev) ? "partition" : "disk", dev->name,
              BLK_SYSFS_MAJOR, (int)index);
}

static void blk_register_internal(struct block_device *dev,
                                  int scan_partitions) {
  /* Normalise whatever the driver reported. Linux does the same in
   * blk_validate_limits(): a queue that names no limit gets the generic
   * defaults rather than a zero that every caller would have to test. */
  if (dev) {
    if (dev->limits.max_sectors == 0)
      dev->limits.max_sectors = BLK_DEF_MAX_SECTORS;
    if (dev->limits.max_segments == 0)
      dev->limits.max_segments = BLK_DEF_MAX_SEGMENTS;
    if (dev->limits.queue_depth == 0)
      dev->limits.queue_depth = BLK_DEF_QUEUE_DEPTH;
  }
  /* Reuse a hole left by an unregistered device before growing the array, so
   * repeated LOOP_CTL_ADD/REMOVE cycles cannot exhaust it. The index is the
   * minor number userspace sees, so it must stay put for everything else. */
  usize index = MAX_BLK_DEVICES;
  for (usize i = 0; i < blk_device_count; i++) {
    if (!blk_devices[i]) {
      index = i;
      break;
    }
  }
  if (index == MAX_BLK_DEVICES && blk_device_count < MAX_BLK_DEVICES)
    index = blk_device_count++;
  if (index == MAX_BLK_DEVICES)
    return; /* registry full */
  blk_devices[index] = dev;
  __atomic_add_fetch(&blk_gen, 1u, __ATOMIC_ACQ_REL);
  /* Publish in /sys BEFORE announcing it: mdev turns the event's DEVPATH
   * straight back into /sys/block/<name>/dev, so the other order leaves a
   * window on SMP where the helper looks the device up before it is there. */
  sysfs_block_changed();
  blk_announce(dev, index, "add");
  if (scan_partitions) {
    blk_scan_partitions(dev);
  }
}

/* Take a device out of the registry: a loop device destroyed through
 * LOOP_CTL_REMOVE, and whatever else grows a removal path later.
 *
 * The slot is left empty rather than compacted, because the index IS the minor
 * number that /sys/dev/block, /proc/partitions and every node userspace has
 * already created name the device by — closing the gap would silently
 * renumber every device above it. The node under /dev is deliberately not
 * touched: on Linux that is udev's (here mdev's) job, and it acts on the
 * remove uevent this raises. */
int blk_unregister(struct block_device *dev) {
  if (!dev)
    return -EINVAL;
  usize index = MAX_BLK_DEVICES;
  for (usize i = 0; i < blk_device_count; i++) {
    if (blk_devices[i] == dev) {
      index = i;
      break;
    }
  }
  if (index == MAX_BLK_DEVICES)
    return -ENODEV;

  /* Nothing may be left referring to this device's blocks. */
  blk_cache_flush(dev);
  blk_cache_invalidate(dev);

  blk_devices[index] = 0;
  /* Trailing holes are not indexes anybody can still be holding, so give them
   * back — that keeps blk_count() honest about the highest live minor. */
  while (blk_device_count > 0 && !blk_devices[blk_device_count - 1])
    blk_device_count--;
  __atomic_add_fetch(&blk_gen, 1u, __ATOMIC_ACQ_REL);
  /* Retire it from /sys first, for the same reason the add publishes first:
   * by the time anything hears about the removal, the tree must agree. */
  sysfs_block_changed();
  blk_announce(dev, index, "remove");
  return 0;
}

/* ── Device naming ──────────────────────────────────────────────────
 *
 * b1nix uses the names every other Unix uses, so ported tools, documentation
 * and habit all land on the right node: SCSI/SATA disks are sdX, virtio disks
 * are vdX, NVMe namespaces are nvme<ctrl>n<nsid>. Nothing here is a table —
 * the suffix is computed from the driver's enumeration index, so the fifth
 * SATA disk comes out sde without anyone editing a list.
 */

/* Bijective base-26 suffix: 0 -> a, 25 -> z, 26 -> aa, 701 -> zz, 702 -> aaa.
 * Same sequence Linux's disk_name() produces. */
void blk_disk_name(const char *prefix, usize index, char *out, usize out_size) {
  if (!out || out_size == 0)
    return;
  char suffix[8];
  usize n = 0;
  long i = (long)index;
  do {
    if (n < sizeof(suffix))
      suffix[n++] = (char)('a' + (i % 26));
    i = i / 26 - 1;
  } while (i >= 0);

  usize w = 0;
  if (prefix) {
    for (const char *p = prefix; *p && w + 1 < out_size; p++)
      out[w++] = *p;
  }
  while (n > 0 && w + 1 < out_size)
    out[w++] = suffix[--n];
  out[w] = '\0';
}

/* NVMe carries the controller index and the namespace id, not a letter:
 * controller 0 namespace 1 is nvme0n1. */
void blk_nvme_name(usize controller, u32 nsid, char *out, usize out_size) {
  if (!out || out_size == 0)
    return;
  snprintf(out, out_size, "nvme%un%u", (unsigned)controller, (unsigned)nsid);
}

/* How many whole disks already answer to <prefix><letters>. This is the next
 * free index in that sequence, read out of the registry rather than kept in a
 * per-driver counter — which is the whole point: AHCI and USB mass storage are
 * both SCSI disks, both named sd*, and two private counters would each hand out
 * sda. Names are never released (a device stays registered for the boot), so a
 * count is exactly the next position. */
static usize blk_next_disk_index(const char *prefix) {
  usize plen = strlen(prefix);
  usize used = 0;
  for (usize i = 0; i < blk_device_count; i++) {
    struct block_device *dev = blk_devices[i];
    if (!dev || !dev->name || blk_is_partition(dev))
      continue;
    if (strncmp(dev->name, prefix, plen) != 0)
      continue;
    const char *suffix = dev->name + plen;
    if (*suffix == '\0')
      continue;
    while (*suffix >= 'a' && *suffix <= 'z')
      suffix++;
    if (*suffix != '\0')
      continue; /* e.g. "sd"-prefixed but not a letter name — not in sequence */
    used++;
  }
  return used;
}

void blk_register_disk(struct block_device *dev, const char *prefix, u8 bus) {
  if (!dev || !prefix)
    return;
  char name[24];
  blk_disk_name(prefix, blk_next_disk_index(prefix), name, sizeof(name));

  usize len = strlen(name) + 1;
  char *persistent = kmalloc(len);
  if (!persistent)
    return;
  memcpy(persistent, name, len);

  dev->name = persistent;
  dev->bus = bus;
  blk_register(dev);
}

struct block_device *blk_nth_on_bus(u8 bus, usize n) {
  usize seen = 0;
  for (usize i = 0; i < blk_device_count; i++) {
    struct block_device *dev = blk_devices[i];
    if (!dev || dev->bus != bus || blk_is_partition(dev))
      continue;
    if (seen == n)
      return dev;
    seen++;
  }
  return 0;
}

int blk_is_removable(struct block_device *dev) {
  return dev && dev->bus == BLK_BUS_USB;
}

/* Partition suffix follows the same rule Linux uses: a bare number appended to
 * the disk name, unless the disk name already ends in a digit — then a 'p'
 * separates them, so nvme0n1's first partition is nvme0n1p1 while sda's is
 * sda1 and there is no ambiguity either way. */
static void make_partition_name(const char *parent, usize number, char *out,
                                usize out_size) {
  usize plen = strlen(parent);
  int ends_in_digit = plen > 0 && parent[plen - 1] >= '0' && parent[plen - 1] <= '9';
  snprintf(out, out_size, "%s%s%u", parent, ends_in_digit ? "p" : "",
           (unsigned)number);
}

static void register_partition(struct block_device *parent, usize number,
                               u64 start_lba, u64 block_count) {
  if (!parent || start_lba == 0 || block_count == 0)
    return;
  struct partition_device *part = 0;
  for (usize i = 0; i < partition_count; i++) {
    if (!partitions[i].parent) {
      part = &partitions[i];
      break;
    }
  }
  if (!part) {
    if (partition_count >= MAX_BLK_PARTITIONS)
      return;
    part = &partitions[partition_count++];
  }
  memset(part, 0, sizeof(*part));
  part->parent = parent;
  part->start_lba = start_lba;

  char name[24];
  make_partition_name(parent->name, number, name, sizeof(name));
  char *persistent_name = kmalloc(strlen(name) + 1);
  if (!persistent_name)
    return;
  memcpy(persistent_name, name, strlen(name) + 1);

  part->blk.name = persistent_name;
  part->blk.bus = parent->bus;
  part->blk.block_size = parent->block_size;
  part->blk.block_count = block_count;
  part->blk.read_blocks = partition_read;
  part->blk.write_blocks = parent->write_blocks ? partition_write : 0;
  /* A partition is the same hardware as its disk — it inherits the disk's
   * per-command limits rather than falling back to the generic defaults. */
  part->blk.limits = parent->limits;
  part->blk.priv = part;
  blk_register_internal(&part->blk, 0);

  console_write("blk: partition ");
  console_write(part->blk.name);
  console_write(" start=");
  console_write_dec(start_lba);
  console_write(" blocks=");
  console_write_dec(block_count);
  console_write("\n");
}

/* ── Partition introspection (backs sysfs /sys/block) ── */

int blk_is_partition(struct block_device *dev) {
  return dev && dev->read_blocks == partition_read;
}

struct block_device *blk_partition_parent(struct block_device *dev) {
  if (!blk_is_partition(dev))
    return 0;
  return ((struct partition_device *)dev->priv)->parent;
}

/* Partition number is the trailing decimal run of the name (1-based), or -1.
 * make_partition_name appends it last in both spellings — sda1 and nvme0n1p1 —
 * so reading the tail back covers each of them. */
int blk_partition_number(struct block_device *dev) {
  if (!blk_is_partition(dev) || !dev->name)
    return -1;
  const char *end = dev->name + strlen(dev->name);
  const char *start = end;
  while (start > dev->name && start[-1] >= '0' && start[-1] <= '9')
    start--;
  if (start == end)
    return -1;
  int num = 0;
  for (const char *c = start; c < end; c++)
    num = num * 10 + (*c - '0');
  return num;
}

/* ── GPT / MBR Scanning ── */

static int scan_gpt(struct block_device *dev, const u8 *mbr) {
  int has_protective = 0;
  for (int i = 0; i < 4; i++) {
    const u8 *entry = mbr + 446 + i * 16;
    if (entry[4] == 0xee)
      has_protective = 1;
  }
  if (!has_protective)
    return 0;

  u8 header[CACHE_BLOCK_SIZE];
  if (blk_read_cached(dev, 1, 1, header) < 0)
    return 0;
  if (memcmp(header, "EFI PART", 8) != 0)
    return 0;

  u64 entries_lba = le64(header + 72);
  u32 entry_count = le32(header + 80);
  u32 entry_size = le32(header + 84);
  if (entry_count == 0 || entry_size < 128 || entry_size > 512)
    return 0;
  if (entry_count > 128)
    entry_count = 128;

  u8 sector[CACHE_BLOCK_SIZE];
  usize found = 0;
  for (u32 i = 0; i < entry_count; i++) {
    u64 byte_offset = (u64)i * entry_size;
    u64 lba = entries_lba + byte_offset / dev->block_size;
    u32 off = (u32)(byte_offset % dev->block_size);
    if (off + entry_size > dev->block_size)
      continue;
    if (blk_read_cached(dev, lba, 1, sector) < 0)
      break;
    const u8 *entry = sector + off;

    int empty = 1;
    for (int b = 0; b < 16; b++) {
      if (entry[b] != 0) {
        empty = 0;
        break;
      }
    }
    if (empty)
      continue;

    u64 first_lba = le64(entry + 32);
    u64 last_lba = le64(entry + 40);
    if (last_lba < first_lba)
      continue;
    register_partition(dev, i + 1, first_lba, last_lba - first_lba + 1);
    found++;
  }
  return found > 0;
}

static int scan_mbr(struct block_device *dev, const u8 *mbr) {
  usize found = 0;
  for (int i = 0; i < 4; i++) {
    const u8 *entry = mbr + 446 + i * 16;
    u8 type = entry[4];
    if (type == 0 || type == 0xee)
      continue;
    u32 start = le32(entry + 8);
    u32 count = le32(entry + 12);
    if (start == 0 || count == 0)
      continue;
    register_partition(dev, i + 1, start, count);
    found++;
  }
  return found > 0;
}

static void blk_scan_partitions(struct block_device *dev) {
  if (!dev || !dev->read_blocks || dev->block_size != CACHE_BLOCK_SIZE)
    return;
  if (dev->block_count != 0 && dev->block_count < 2)
    return;

  u8 mbr[CACHE_BLOCK_SIZE];
  if (blk_read_cached(dev, 0, 1, mbr) < 0)
    return;
  if (mbr[510] != 0x55 || mbr[511] != 0xaa)
    return;

  if (scan_gpt(dev, mbr))
    return;
  scan_mbr(dev, mbr);
}

int blk_rescan_partitions(struct block_device *dev) {
  if (!dev || blk_is_partition(dev))
    return -1;

  usize out = 0;
  for (usize i = 0; i < blk_device_count; i++) {
    struct block_device *candidate = blk_devices[i];
    if (blk_is_partition(candidate) && blk_partition_parent(candidate) == dev) {
      struct partition_device *part = (struct partition_device *)candidate->priv;
      if (candidate->name)
        kfree((void *)candidate->name);
      memset(part, 0, sizeof(*part));
      continue;
    }
    blk_devices[out++] = candidate;
  }
  blk_device_count = out;
  /* The compaction above removed devices without going through
   * blk_unregister(), so nothing has told sysfs yet — and it also shifts the
   * index (the minor) of everything that followed them. */
  __atomic_add_fetch(&blk_gen, 1u, __ATOMIC_ACQ_REL);
  sysfs_block_changed();
  blk_cache_invalidate(dev);
  blk_scan_partitions(dev);
  blk_create_dev_nodes();
  return 0;
}

void blk_register(struct block_device *dev) { blk_register_internal(dev, 1); }

struct block_device *blk_get(const char *name) {
  for (usize i = 0; i < blk_device_count; i++) {
    /* A slot emptied by blk_unregister() reads back NULL — the registry has
     * holes now, and every walk of it has to expect one. */
    if (blk_devices[i] && blk_devices[i]->name &&
        strcmp(blk_devices[i]->name, name) == 0) {
      return blk_devices[i];
    }
  }
  return 0;
}

usize blk_count(void) { return blk_device_count; }

/* Device number for a registered block device, in the classic dev_t packing
 * (major << 8 | minor) that /proc/<pid>/maps and stat's st_dev use. The major
 * follows the device class the way Linux assigns it — a tool that recognises
 * 8 as "SCSI/SATA disk" or 259 as "NVMe" reads b1nix's numbers correctly — and
 * the minor is the device's registration index, which is stable for a boot.
 * Returns 0 for a device that is not registered (no such dev_t exists). */
u32 blk_devno(struct block_device *dev) {
  if (!dev || !dev->name)
    return 0;
  usize index = 0;
  int found = 0;
  for (usize i = 0; i < blk_device_count; i++) {
    if (blk_devices[i] == dev) {
      index = i;
      found = 1;
      break;
    }
  }
  if (!found)
    return 0;

  u32 major;
  const char *n = dev->name;
  if (strncmp(n, "nvme", 4) == 0)
    major = 259; /* nvme */
  else if (strncmp(n, "sd", 2) == 0)
    major = 8; /* SCSI/SATA disk */
  else if (strncmp(n, "vd", 2) == 0)
    major = 254; /* virtio-blk */
  else if (strncmp(n, "ram", 3) == 0)
    major = 1; /* ramdisk */
  else if (strncmp(n, "loop", 4) == 0)
    major = 7; /* loop */
  else
    major = 240; /* local/experimental range for anything else */
  return (major << 8) | (u32)(index & 0xFF);
}

struct block_device *blk_at(usize index) {
  if (index >= blk_device_count)
    return 0;
  return blk_devices[index];
}

static int blk_read_bytes(struct block_device *dev, u64 offset, void *buffer,
                          usize size) {
  if (!dev || !dev->read_blocks || dev->block_size == 0 || size == 0)
    return -1;
  if (dev->block_count > 0) {
    u64 device_size = dev->block_count * (u64)dev->block_size;
    if (offset >= device_size || size > device_size - offset)
      return -1;
  }

  u8 *block = kmalloc(dev->block_size);
  if (!block)
    return -1;

  u8 *out = buffer;
  while (size > 0) {
    u64 lba = offset / dev->block_size;
    usize within = (usize)(offset % dev->block_size);
    usize chunk = dev->block_size - within;
    if (chunk > size)
      chunk = size;
    if (blk_read_cached(dev, lba, 1, block) < 0) {
      kfree(block);
      return -1;
    }
    memcpy(out, block + within, chunk);
    out += chunk;
    offset += chunk;
    size -= chunk;
  }

  kfree(block);
  return 0;
}

const char *blk_probe_fstype(struct block_device *dev) {
  u8 boot[512];
  if (blk_read_bytes(dev, 0, boot, sizeof(boot)) < 0)
    return "-";

  if (memcmp(boot + 3, "NTFS    ", 8) == 0)
    return "ntfs";
  if (memcmp(boot + 3, "EXFAT   ", 8) == 0)
    return "exfat";
  if (memcmp(boot + 82, "FAT32   ", 8) == 0)
    return "fat32";
  if (memcmp(boot + 54, "FAT16   ", 8) == 0)
    return "fat16";
  if (memcmp(boot + 54, "FAT12   ", 8) == 0)
    return "fat12";

  u8 ext_sb[104];
  if (blk_read_bytes(dev, 1024, ext_sb, sizeof(ext_sb)) == 0 &&
      le16(ext_sb + 0x38) == 0xef53) {
    u32 compat = le32(ext_sb + 0x5c);
    u32 incompat = le32(ext_sb + 0x60);
    if (incompat & (0x0040u | 0x0080u | 0x0100u | 0x0200u |
                    0x4000u | 0x8000u))
      return "ext4";
    if (compat & 0x0004u)
      return "ext3";
    return "ext2";
  }

  u8 magic[8];
  if (blk_read_bytes(dev, 65536 + 0x40, magic, sizeof(magic)) == 0 &&
      memcmp(magic, "_BHRfS_M", 8) == 0)
    return "btrfs";

  u8 iso_magic[5];
  if (blk_read_bytes(dev, 32768 + 1, iso_magic, sizeof(iso_magic)) == 0 &&
      memcmp(iso_magic, "CD001", 5) == 0)
    return "iso9660";

  return "-";
}

/* ── Volume identity: UUID and label ──
 *
 * What `findfs UUID=…`, `root=LABEL=…` and `lsblk -o UUID` are asking for. The
 * ext lookup used to live twice in kernel/main.c, once for UUID and once for
 * label, each opening the superblock itself and each understanding ext only.
 * It belongs beside blk_probe_fstype: same read, same question — what is on
 * this device — and one implementation covers FAT and exFAT as well.
 *
 * Both return 0 on success and fill a NUL-terminated string; -1 when the
 * device carries nothing that has the field.
 */
static void hex2(char *out, u8 v) {
  static const char digits[] = "0123456789abcdef";
  out[0] = digits[(v >> 4) & 0xF];
  out[1] = digits[v & 0xF];
}

/* Trim the trailing padding a fixed-width on-disk label field carries. */
static void label_copy(char *out, usize cap, const char *raw, usize raw_len) {
  usize n = raw_len < cap - 1 ? raw_len : cap - 1;
  while (n > 0 && (raw[n - 1] == ' ' || raw[n - 1] == '\0'))
    n--;
  memcpy(out, raw, n);
  out[n] = '\0';
}

int blk_probe_uuid(struct block_device *dev, char *out, usize cap) {
  if (!dev || !out || cap < 37)
    return -1;
  out[0] = '\0';

  /* Big enough to reach s_volume_name at 0x78+16: a 104-byte read (which is
     all blk_probe_fstype needs) stops short of s_uuid at 0x68 entirely. */
  u8 ext_sb[152];
  if (blk_read_bytes(dev, 1024, ext_sb, sizeof(ext_sb)) == 0 &&
      le16(ext_sb + 0x38) == 0xef53) {
    /* s_uuid is at offset 0x68 of the ext superblock, printed in the canonical
     * 8-4-4-4-12 form findfs and blkid compare against. */
    const u8 *u = ext_sb + 0x68;
    int p = 0;
    for (int i = 0; i < 16; i++) {
      if (i == 4 || i == 6 || i == 8 || i == 10)
        out[p++] = '-';
      hex2(out + p, u[i]);
      p += 2;
    }
    out[p] = '\0';
    return 0;
  }

  u8 boot[512];
  if (blk_read_bytes(dev, 0, boot, sizeof(boot)) < 0)
    return -1;

  /* exFAT's volume serial is 32 bits at offset 100, FAT32's at 67 and
   * FAT12/16's at 39 — all printed the way blkid prints them, XXXX-XXXX. */
  const u8 *serial = 0;
  if (memcmp(boot + 3, "EXFAT   ", 8) == 0)
    serial = boot + 100;
  else if (memcmp(boot + 82, "FAT32   ", 8) == 0)
    serial = boot + 67;
  else if (memcmp(boot + 54, "FAT16   ", 8) == 0 ||
           memcmp(boot + 54, "FAT12   ", 8) == 0)
    serial = boot + 39;
  if (!serial)
    return -1;

  hex2(out + 0, serial[3]);
  hex2(out + 2, serial[2]);
  out[4] = '-';
  hex2(out + 5, serial[1]);
  hex2(out + 7, serial[0]);
  out[9] = '\0';
  return 0;
}

int blk_probe_label(struct block_device *dev, char *out, usize cap) {
  if (!dev || !out || cap < 2)
    return -1;
  out[0] = '\0';

  u8 ext_sb[152];
  if (blk_read_bytes(dev, 1024, ext_sb, sizeof(ext_sb)) == 0 &&
      le16(ext_sb + 0x38) == 0xef53) {
    /* s_volume_name: 16 bytes at 0x78. */
    label_copy(out, cap, (const char *)(ext_sb + 0x78), 16);
    return out[0] ? 0 : -1;
  }

  u8 boot[512];
  if (blk_read_bytes(dev, 0, boot, sizeof(boot)) < 0)
    return -1;
  const char *raw = 0;
  if (memcmp(boot + 82, "FAT32   ", 8) == 0)
    raw = (const char *)(boot + 71);
  else if (memcmp(boot + 54, "FAT16   ", 8) == 0 ||
           memcmp(boot + 54, "FAT12   ", 8) == 0)
    raw = (const char *)(boot + 43);
  if (!raw)
    return -1;
  label_copy(out, cap, raw, 11);
  return out[0] ? 0 : -1;
}

/* ── Write-Back Cache Implementation ── */

static void blk_io_gate_init(void);

void blk_cache_init(void) {
  blk_io_gate_init();
  /* Scale pool to RAM: ~1 entry per 512 KiB of usable memory, clamped. */
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  /* 8 entries per MiB of RAM — 4 KiB of cache per MiB, i.e. 0.4% of memory.
   * At 2 per MiB the pool stayed at 4 MiB on any machine large enough for the
   * ceiling to matter, which is where the 96%-miss measurement came from. */
  /* 32 entries per MiB — 16 KiB of cache per MiB, i.e. 1.6% of memory.
   *
   * read(2) on a regular file does not go through the file page cache at all;
   * it reads blocks, so this pool IS the read cache for every program that uses
   * read rather than mmap. At 8 entries per MiB it was 16 MB against a browser
   * whose working set is hundreds, and every read(2) cost a disk round trip. */
  usize want = (usize)(ram_mb * 32);
  /* `b1nix.bcache-mb=N` states the cache size directly, in mebibytes, for the
   * cases where the RAM fraction is the wrong answer — a memory-starved run
   * that still wants a big cache, or the reverse. */
  u32 cache_mb = bootinfo_get_u32("b1nix.bcache-mb", 0);
  if (cache_mb)
    want = (usize)((u64)cache_mb * 1024ULL * 1024ULL / sizeof(struct block_buffer));
  if (want < CACHE_ENTRIES_MIN) want = CACHE_ENTRIES_MIN;
  if (want > CACHE_ENTRIES_MAX) want = CACHE_ENTRIES_MAX;
  block_cache_n = want;
  /* Read-ahead default in kilobytes, the unit Linux uses for read_ahead_kb. */
  u32 ra_kb = bootinfo_get_u32("b1nix.read-ahead-kb", BCACHE_READAHEAD_KB_DEF);
  bcache_ra_sectors = ra_kb * 1024u / CACHE_BLOCK_SIZE;
  block_cache = kzalloc(block_cache_n * sizeof(struct block_buffer));
  if (!block_cache) {
    /* Fall back to floor — kzalloc still failed? then we have bigger problems
     * but try a smaller pool before giving up entirely. */
    block_cache_n = CACHE_ENTRIES_MIN;
    block_cache = kzalloc(block_cache_n * sizeof(struct block_buffer));
  }
  /* One bucket per four entries, rounded up to a power of two and bounded so
   * the table is never smaller than a kilobyte's worth of buckets nor larger
   * than 256 KiB. */
  u32 bits = BCACHE_HASH_MIN_BITS;
  while (((usize)1u << bits) < BCACHE_HASH_MAX_ENTRIES &&
         ((usize)1u << bits) < block_cache_n / 4u)
    bits++;
  u32 buckets = 1u << bits;
  bcache_hash = (i32 *)kzalloc((usize)buckets * sizeof(i32));
  if (!bcache_hash) {
    buckets = 1u << BCACHE_HASH_MIN_BITS;
    bcache_hash = (i32 *)kzalloc((usize)buckets * sizeof(i32));
  }
  bcache_hash_mask = buckets - 1u;
  /* kzalloc zeroes the buffer, but hash_next must start at -1 (empty), not 0
   * (which is a valid index). Likewise prime the hash buckets to -1. */
  for (usize i = 0; i < block_cache_n; i++) block_cache[i].hash_next = -1;
  for (u32 b = 0; b < buckets; b++) bcache_hash[b] = -1;
  console_write("blk-cache: ");
  console_write_dec(block_cache_n);
  console_write(" entries (");
  console_write_dec((block_cache_n * sizeof(struct block_buffer)) / 1024);
  console_write(" KiB), ");
  console_write_dec(buckets);
  console_write(" hash buckets, read-ahead ");
  console_write_dec(bcache_ra_sectors * CACHE_BLOCK_SIZE / 1024);
  console_write(" KiB\n");
}

static struct block_buffer *bcache_find(struct block_device *dev, u64 lba) {
  /* O(chain-length) lookup via the (bdev, lba)-keyed hash table. Matches both
   * VALID entries and in-progress BUSY entries: a slot mid-fill or mid-eviction
   * is hash-linked under its key so a concurrent miss on the same key finds it
   * and waits, instead of evicting a second slot and creating a DUPLICATE entry
   * for one (dev,lba) — the F2 stale-data bug. Callers must therefore check
   * BLK_CACHE_BUSY before reading the data. */
  u32 h = bcache_bucket(dev, lba);
  i32 idx = bcache_hash[h];
  while (idx >= 0) {
    struct block_buffer *e = &block_cache[idx];
    if ((e->flags & (BLK_CACHE_VALID | BLK_CACHE_BUSY)) &&
        e->bdev == dev && e->block_no == lba) {
      e->last_used = ++bcache_tick;
      return e;
    }
    idx = e->hash_next;
  }
  return 0;
}

/* Pick + prepare a slot to evict. Caller holds bcache_lock; on dirty entries we
 * MUST drop the lock around write_blocks (the virtio/AHCI drivers spin on
 * scheduler_yield(), and CLAUDE.md's "Never Sleep While Holding a Spinlock"
 * rule means yielding under bcache_lock is a deadlock waiting to happen — and
 * also misleads vfs_inode_lock_*'s "is bcache held by this CPU?" guard, so a
 * task that runs during the yield trips the lock-order panic). The slot is
 * claimed via BLK_CACHE_BUSY for the duration so a concurrent evict on
 * another CPU skips it. flags_inout lets us release/reacquire with the
 * caller's saved IRQ state preserved. */
/* CLOCK eviction hand — rotates across block_cache[] so victim selection is
 * O(1) amortized instead of two full-pool scans per miss. */
static usize bcache_hand = 0;

static struct block_buffer *bcache_evict(u64 *flags_inout) {
  /* CLOCK-style victim selection. The previous implementation scanned the
   * whole pool twice on EVERY miss (an empty-slot pass, then a global-LRU
   * pass) — an O(n) cost that, once read-ahead cut the DMA-command count,
   * became the dominant term (~130 ms / 16 MiB ⇒ a ~120 MB/s ceiling). The
   * rotating hand instead takes the first usable slot it sweeps past:
   *   - an empty (never-filled/invalidated) non-busy slot is taken as-is;
   *   - else the first non-busy CLEAN valid slot (no write-back);
   *   - else, only if every valid slot is dirty, the first non-busy dirty one
   *     (write-back outside the lock, as before).
   * Streaming a binary far larger than the cache evicts cold, clean slots, so
   * the hand finds a victim in ~1 step. Approximating LRU with CLOCK is the
   * standard trade and is correct for any reference pattern. */
  int victim = -1;
  int dirty_victim = -1;
  for (usize scanned = 0; scanned < block_cache_n; scanned++) {
    usize i = bcache_hand;
    bcache_hand = (bcache_hand + 1 >= block_cache_n) ? 0 : bcache_hand + 1;
    struct block_buffer *e = &block_cache[i];
    if (e->flags & BLK_CACHE_BUSY)
      continue;
    if (!(e->flags & BLK_CACHE_VALID))
      return e; /* empty, non-busy — best case, no write-back */
    if (!(e->flags & BLK_CACHE_DIRTY)) {
      victim = (int)i; /* first clean valid slot — evict it */
      break;
    }
    if (dirty_victim < 0)
      dirty_victim = (int)i; /* remember a dirty fallback, keep seeking clean */
  }
  if (victim < 0)
    victim = dirty_victim; /* all valid slots dirty — evict the first one */
  if (victim < 0)
    return 0; /* all entries in-flight — caller yields and retries */

  int oldest_idx = victim;
  struct block_buffer *entry = &block_cache[oldest_idx];

  /* 3. Write-Back: flush a dirty entry to disk OUTSIDE the bcache_lock. */
  if ((entry->flags & BLK_CACHE_DIRTY) && entry->bdev && entry->bdev->write_blocks) {
    entry->flags |= BLK_CACHE_BUSY;          /* lock the slot across the drop */
    struct block_device *wb_dev = entry->bdev;
    u64 wb_lba = entry->block_no;
    bcache_release(*flags_inout);            /* RELEASE — write_blocks may yield */
    /* Eviction write-back: the slot is recycled regardless (the caller resets
     * its flags below), so a failed write here loses the data under memory
     * pressure — a documented limitation. The sync/fsync path in
     * blk_flush_buffer keeps DIRTY on failure so explicit syncs don't lose
     * data silently (R3-13). */
    blk_dev_write(wb_dev, wb_lba, 1, entry->data);
    *flags_inout = bcache_acquire();         /* REACQUIRE before returning */
    entry->flags &= ~(BLK_CACHE_DIRTY | BLK_CACHE_BUSY);
  }
  /* Unlink from its old hash chain — its (bdev, block_no) is about to be
   * replaced. Leaving it linked would leak chain length and waste lookups. */
  bcache_hash_remove((i32)oldest_idx);
  entry->flags &= ~BLK_CACHE_VALID;
  return entry;
}


/* ── I/O priority gate (ioprio_set(2)) ───────────────────────────────────────
 * b1nix issues one device command at a time per device: the drivers serialise
 * internally and the block cache marks an in-flight sector BUSY. That
 * serialisation point is the only place where request order is decided, so it
 * is where ioprio has to act. A request takes the gate before touching the
 * device; if the device is busy the task parks, and when the holder leaves it
 * hands the gate to the WAITING TASK WITH THE BEST I/O PRIORITY rather than to
 * whoever wakes first. With no contention this is two atomic operations and no
 * ordering decision at all, so the fast path is unchanged.
 *
 * Priority follows the Linux encoding: class in bits 13..15 (1 = realtime,
 * 2 = best-effort, 3 = idle) and level in bits 0..12, lower is better. A task
 * that never called ioprio_set has value 0, which sorts as best-effort level 0
 * — exactly how Linux treats an unset priority. */
/* How many tasks can queue on one device's priority gate.
 *
 * Overflowing this is not a correctness failure — the extra waiters fall back
 * to spinning on the gate — but it silently discards the I/O priority ordering
 * the gate exists to provide, which is a wall you cannot see from userspace.
 * Sixteen was fewer slots than a threaded program keeps requests in flight.
 *
 * FLOOR   16 waiters, the previous fixed depth: a uniprocessor guest is
 *         unchanged.
 * CEILING 256 waiters — 4 KiB per gate, and past that the linear scan
 *         blk_io_end() does to pick the best waiter costs more than the
 *         ordering is worth.
 * Scaled by CPU count because that is what bounds how many tasks can be inside
 * blk_io_begin at once. `b1nix.io-waiters=N` overrides. */
#define BLK_IO_WAITERS_MIN 16u
#define BLK_IO_WAITERS 256u /* ceiling; the live depth is blk_io_waiters */
static u32 blk_io_waiters = BLK_IO_WAITERS_MIN;

/* Deepest the queue on any one gate has ever been, and how often it filled.
 * Measured rather than assumed: the report at the end of a run is what says
 * whether the old fixed depth was a real limit on this workload or not. */
static u32 blk_gate_peak;
static u32 blk_gate_full;
/* Report those two on the serial log in test mode. A peak can only rise
 * blk_io_waiters times, and the table-full line is printed once, so this is a
 * handful of lines per boot at most. */
static int blk_limits_verbose;

struct blk_io_waiter {
  struct task *task;
  int prio;       /* effective priority: class<<13 | level, lower is better */
  u64 queued_at;  /* tick the request started waiting — drives ageing */
  int admitted;
  int used;
};

struct blk_io_gate {
  struct block_device *dev;
  int busy;
  u32 depth; /* waiters currently queued — see blk_gate_peak */
  struct blk_io_waiter waiters[BLK_IO_WAITERS];
};

/* Pick the gate depth for this machine. Called once from blk_cache_init. */
static void blk_io_gate_init(void) {
  /* Four waiters per CPU: enough that the tasks a CPU can have parked on one
   * device all keep their place in the priority order. ACPI has enumerated the
   * CPUs by the time the block layer initialises, even though the APs are not
   * running yet, so this counts every CPU that will ever exist. */
  int cpus = acpi_cpu_count();
  u32 want = bootinfo_get_u32("b1nix.io-waiters",
                              (u32)(cpus > 0 ? cpus : 1) * 4u);

  if (want < BLK_IO_WAITERS_MIN)
    want = BLK_IO_WAITERS_MIN;
  if (want > BLK_IO_WAITERS)
    want = BLK_IO_WAITERS;
  blk_io_waiters = want;
  blk_limits_verbose = bootinfo_has_flag("b1nix.test=1");
}

static struct blk_io_gate blk_gates[MAX_BLK_DEVICES];
static spinlock_t blk_gate_lock = SPINLOCK_INIT;

/* Ageing: a request that has waited this many ticks gains one priority level,
 * so a stream of best-effort I/O cannot starve an idle-class request forever.
 * One tick is 10 ms; a full class (8192 levels) is therefore reachable in well
 * under a second of continuous starvation, which is the point — the ordering
 * must be a preference, not a veto. */
#define BLK_IO_AGE_TICKS 2
#define BLK_IO_AGE_STEP 512

/* Effective priority of a waiter right now: its own priority, improved by how
 * long it has been waiting. Never goes below 0 (better than realtime level 0),
 * so ageing can promote a waiter past a class but not below the floor. */
static int blk_io_effective_prio(const struct blk_io_waiter *w, u64 now) {
  u64 waited = (now > w->queued_at) ? (now - w->queued_at) : 0;
  u64 boost = (waited / BLK_IO_AGE_TICKS) * BLK_IO_AGE_STEP;
  if (boost >= (u64)w->prio)
    return 0;
  return w->prio - (int)boost;
}

static struct blk_io_gate *blk_gate_of(struct block_device *dev) {
  for (usize i = 0; i < MAX_BLK_DEVICES; i++)
    if (blk_gates[i].dev == dev)
      return &blk_gates[i];
  for (usize i = 0; i < MAX_BLK_DEVICES; i++)
    if (!blk_gates[i].dev) {
      blk_gates[i].dev = dev;
      return &blk_gates[i];
    }
  return 0;
}

/* Effective priority: class first, then level. Class 0 (unset) is treated as
 * best-effort, matching Linux. */
static int blk_io_prio_of_current(void) {
  int raw = scheduler_get_ioprio(0);
  if (raw < 0)
    raw = 0;
  int class = (raw >> 13) & 0x7;
  int level = raw & 0x1fff;
  if (class == 0)
    class = 2; /* IOPRIO_CLASS_BE */
  return (class << 13) | level;
}

static void blk_io_begin(struct block_device *dev) {
  if (!dev || !current_task)
    return;
  u64 flags;
  spin_lock_irqsave(&blk_gate_lock, &flags);
  struct blk_io_gate *g = blk_gate_of(dev);
  if (!g) {
    spin_unlock_irqrestore(&blk_gate_lock, flags);
    return;
  }
  if (!g->busy) {
    g->busy = 1;
    spin_unlock_irqrestore(&blk_gate_lock, flags);
    return;
  }
  int slot = -1;
  for (u32 i = 0; i < blk_io_waiters; i++) {
    if (!g->waiters[i].used) {
      g->waiters[i].used = 1;
      g->waiters[i].task = current_task;
      g->waiters[i].prio = blk_io_prio_of_current();
      g->waiters[i].queued_at = scheduler_get_uptime_ticks();
      g->waiters[i].admitted = 0;
      slot = (int)i;
      break;
    }
  }
  /* Queue depth, kept as a counter rather than recomputed: this is the hot
   * path, and the point of the scan above is to stop at the first free slot.
   * Everything here runs under blk_gate_lock, so plain arithmetic is safe. */
  int report_peak = 0, report_full = 0;

  if (slot >= 0) {
    if (++g->depth > blk_gate_peak) {
      blk_gate_peak = g->depth;
      report_peak = 1;
    }
  } else if (++blk_gate_full == 1) {
    report_full = 1;
  }
  u32 peak = blk_gate_peak;
  spin_unlock_irqrestore(&blk_gate_lock, flags);

  if (blk_limits_verbose && report_peak) {
    console_write("blk: io-gate depth ");
    console_write_dec(peak);
    console_write(" of ");
    console_write_dec(blk_io_waiters);
    console_write("\n");
  }
  if (blk_limits_verbose && report_full) {
    console_write("blk: io-gate full at ");
    console_write_dec(blk_io_waiters);
    console_write(" waiters - priority order dropped\n");
  }
  if (slot < 0) {
    /* More concurrent requests than the wait table holds: fall back to
     * spinning on the gate rather than losing the ordering guarantee. */
    while (1) {
      spin_lock_irqsave(&blk_gate_lock, &flags);
      if (!g->busy) {
        g->busy = 1;
        spin_unlock_irqrestore(&blk_gate_lock, flags);
        return;
      }
      spin_unlock_irqrestore(&blk_gate_lock, flags);
      scheduler_yield();
    }
  }
  while (1) {
    spin_lock_irqsave(&blk_gate_lock, &flags);
    if (g->waiters[slot].admitted) {
      g->waiters[slot].used = 0;
      if (g->depth)
        g->depth--;
      g->waiters[slot].admitted = 0;
      spin_unlock_irqrestore(&blk_gate_lock, flags);
      return; /* the gate was handed to us, still held */
    }
    spin_unlock_irqrestore(&blk_gate_lock, flags);
    scheduler_yield();
  }
}

static void blk_io_end(struct block_device *dev) {
  if (!dev || !current_task)
    return;
  u64 flags;
  spin_lock_irqsave(&blk_gate_lock, &flags);
  struct blk_io_gate *g = blk_gate_of(dev);
  if (!g) {
    spin_unlock_irqrestore(&blk_gate_lock, flags);
    return;
  }
  u64 now = scheduler_get_uptime_ticks();
  int best = -1, best_prio = 0;
  for (u32 i = 0; i < blk_io_waiters; i++) {
    if (!g->waiters[i].used || g->waiters[i].admitted)
      continue;
    int p = blk_io_effective_prio(&g->waiters[i], now);
    if (best < 0 || p < best_prio) {
      best = i;
      best_prio = p;
    }
  }
  if (best >= 0) {
    /* Hand the gate straight over: it stays busy, and the chosen waiter is the
     * highest-priority one rather than the first to be scheduled. */
    g->waiters[best].admitted = 1;
    struct task *next = g->waiters[best].task;
    spin_unlock_irqrestore(&blk_gate_lock, flags);
    if (next)
      scheduler_wake_all(next);
    return;
  }
  g->busy = 0;
  spin_unlock_irqrestore(&blk_gate_lock, flags);
}

/* Self-test of the admission policy (b1nix.test=1). Drives the real waiter
 * table and the real chooser with synthetic entries, so what is checked is the
 * code the I/O path runs: better priority wins, and a long-waiting idle-class
 * request eventually overtakes a fresh best-effort one instead of starving.
 * Returns 0 on success, or the number of the check that failed. */
int blk_io_gate_selftest(void) {
  struct blk_io_gate g;
  memset(&g, 0, sizeof(g));
  u64 now = 1000;

  /* Three waiters queued at the same moment: realtime, best-effort, idle. */
  g.waiters[0] = (struct blk_io_waiter){0, (1 << 13) | 4, now, 0, 1}; /* RT   */
  g.waiters[1] = (struct blk_io_waiter){0, (2 << 13) | 0, now, 0, 1}; /* BE   */
  g.waiters[2] = (struct blk_io_waiter){0, (3 << 13) | 0, now, 0, 1}; /* IDLE */

  int best = -1, best_prio = 0;
  for (int i = 0; i < 3; i++) {
    int p = blk_io_effective_prio(&g.waiters[i], now);
    if (best < 0 || p < best_prio) { best = i; best_prio = p; }
  }
  if (best != 0)
    return 1; /* realtime must be admitted first */

  g.waiters[0].used = 0; /* it ran */
  best = -1;
  for (int i = 0; i < 3; i++) {
    if (!g.waiters[i].used) continue;
    int p = blk_io_effective_prio(&g.waiters[i], now);
    if (best < 0 || p < best_prio) { best = i; best_prio = p; }
  }
  if (best != 1)
    return 2; /* then best-effort, ahead of idle */

  /* Now let the idle request wait while best-effort requests keep arriving:
   * after enough ticks the aged idle request must win. */
  u64 later = now + 40;
  g.waiters[1].queued_at = later; /* a freshly queued best-effort request */
  int idle_p = blk_io_effective_prio(&g.waiters[2], later);
  int be_p = blk_io_effective_prio(&g.waiters[1], later);
  if (idle_p >= be_p)
    return 3; /* ageing failed — the idle request would starve */
  return 0;
}

/* The device-command wrappers every path below goes through. */
static int blk_dev_read(struct block_device *dev, u64 lba, u32 count,
                        void *buf) {
  blk_io_begin(dev);
  int rc = dev->read_blocks(dev, lba, count, buf);
  blk_io_end(dev);
  return rc;
}

static int blk_dev_write(struct block_device *dev, u64 lba, u32 count,
                         const void *buf) {
  blk_io_begin(dev);
  int rc = dev->write_blocks(dev, lba, count, buf);
  blk_io_end(dev);
  return rc;
}

/* Partitions are cached under their parent, at the parent's LBA. A partition
 * read used to re-enter the cache one level down, so every sector held two
 * entries — one keyed by the partition, one by the disk — and each eviction
 * and read-ahead ran twice. Worse, blk_cache_flush(partition) matched only the
 * partition-keyed copies, and writing those back merely re-dirtied the
 * parent-keyed layer, so fsync() on a partition-mounted root reported success
 * without anything reaching the platter. Resolving here gives one keying. */
static struct block_device *blk_cache_target(struct block_device *dev,
                                             u64 *lba) {
  if (blk_is_partition(dev)) {
    struct partition_device *part = (struct partition_device *)dev->priv;
    if (part && part->parent) {
      *lba += part->start_lba;
      return part->parent;
    }
  }
  return dev;
}

int blk_read_cached(struct block_device *dev, u64 lba, u32 count,
                    void *buffer) {
  if (!dev || !dev->read_blocks) {
    k_info("blk_read_cached", "dev or read_blocks is NULL");
    return -1;
  }
  if (dev->block_count > 0 && (count > dev->block_count || lba > dev->block_count - count)) {
    console_write("blk_read_cached: bounds check failed for ");
    console_write(dev->name);
    console_write(" lba="); console_write_dec(lba);
    console_write(" count="); console_write_dec(count);
    console_write(" block_count="); console_write_dec(dev->block_count);
    console_write("\n");
    return -1;
  }
  if (dev->block_size != CACHE_BLOCK_SIZE) {
    return blk_dev_read(dev, lba, count, buffer);
  }
  /* Bounds were checked against the partition above; from here the request is
   * the parent's. */
  dev = blk_cache_target(dev, &lba);

  u8 *buf8 = (u8 *)buffer;
  for (u32 i = 0; i < count; i++) {
    u64 current_lba = lba + i;
    /* Retry loop: re-runs only if every cache slot is momentarily in-flight on
     * other CPUs (bcache_evict returned NULL) — otherwise breaks after one pass. */
    for (;;) {
      u64 flags = bcache_acquire();
      struct block_buffer *entry = bcache_find(dev, current_lba);

      if (entry) {
        /* In-progress (mid-fill or mid-eviction): wait, don't read garbage or
         * spawn a duplicate. */
        if (entry->flags & BLK_CACHE_BUSY) {
          bcache_release(flags);
          scheduler_yield();
          continue;
        }
        memcpy(buf8 + i * CACHE_BLOCK_SIZE, entry->data, CACHE_BLOCK_SIZE);
        bcache_release(flags);
        break;
      }

      entry = bcache_evict(&flags);
      if (!entry) {
        bcache_release(flags);
        scheduler_yield(); /* all slots in-flight; let a filler finish */
        continue;
      }
      /* bcache_evict may have dropped the lock for a dirty write-back; another
       * CPU could have filled this key meanwhile. Re-check and, if so, abandon
       * our (now free/invalid) slot and retry to use the winner. */
      if (bcache_find(dev, current_lba)) {
        bcache_release(flags);
        continue;
      }
      entry->bdev = dev;
      entry->block_no = current_lba;
      /* Claim the slot AND publish it in the hash as in-progress (BUSY, not yet
       * VALID) BEFORE dropping the lock for the DMA, so a concurrent miss on
       * the same key finds it via bcache_find and waits instead of filling a
       * second slot. */
      entry->flags |= BLK_CACHE_BUSY;
      entry->flags &= ~(BLK_CACHE_VALID | BLK_CACHE_DIRTY);
      bcache_hash_insert((i32)(entry - block_cache));
      bcache_release(flags);

      /* Read-ahead: pull a contiguous run starting at current_lba in ONE device
       * command rather than one DMA per sector. The claimed slot above only
       * covers current_lba; the remaining sectors of the run are inserted into
       * the cache below from the buffer we already have — no extra DMA. Falls
       * back to a single-sector read if the run buffer can't be allocated. */
      u32 run = blk_readahead_for(dev, current_lba);
      if (dev->block_count > 0 && current_lba + run > dev->block_count)
        run = (u32)(dev->block_count - current_lba);
      if (run == 0)
        run = 1;
      u8 *ra = (run > 1) ? (u8 *)kmalloc((usize)run * CACHE_BLOCK_SIZE) : 0;
      int rc;
      if (ra) {
        rc = blk_dev_read(dev, current_lba, run, ra);
        if (rc >= 0)
          memcpy(entry->data, ra, CACHE_BLOCK_SIZE);
      } else {
        run = 1;
        rc = blk_dev_read(dev, current_lba, 1, entry->data);
      }
      if (rc < 0) {
        if (ra)
          kfree(ra);
        flags = bcache_acquire();
        /* Unpublish the failed in-progress slot and free it. */
        bcache_hash_remove((i32)(entry - block_cache));
        entry->flags &= ~(BLK_CACHE_BUSY | BLK_CACHE_VALID);
        bcache_release(flags);
        console_write("blk_read_cached: read_blocks failed for ");
        console_write(dev->name);
        console_write(" lba="); console_write_dec(current_lba);
        console_write("\n");
        return -1;
      }

      flags = bcache_acquire();
      entry->flags |= BLK_CACHE_VALID;
      entry->flags &= ~(BLK_CACHE_DIRTY | BLK_CACHE_BUSY);
      entry->last_used = ++bcache_tick;
      /* Already hash-linked (as BUSY) before the DMA. */
      memcpy(buf8 + i * CACHE_BLOCK_SIZE, entry->data, CACHE_BLOCK_SIZE);

      /* Populate the read-ahead sectors (1..run-1) into the cache from the run
       * buffer already in hand — no extra DMA. Best-effort: skip any key that
       * is already cached or in-flight, and stop if every slot is busy. Uses
       * the same LRU eviction as a normal miss (bcache_evict), so this never
       * costs more device commands than the pre-read-ahead path did. */
      for (u32 j = 1; j < run; j++) {
        u64 ra_lba = current_lba + j;
        if (bcache_find(dev, ra_lba))
          continue; /* already cached or being filled by someone else */
        struct block_buffer *slot = bcache_evict(&flags);
        if (!slot)
          break; /* all slots in-flight — stop prefetch */
        /* bcache_evict may have dropped the lock for a dirty write-back; the
         * key could have been filled meanwhile — don't create a duplicate. */
        if (bcache_find(dev, ra_lba))
          continue;
        slot->bdev = dev;
        slot->block_no = ra_lba;
        slot->flags |= BLK_CACHE_VALID;
        slot->flags &= ~(BLK_CACHE_BUSY | BLK_CACHE_DIRTY);
        slot->last_used = ++bcache_tick;
        memcpy(slot->data, ra + (usize)j * CACHE_BLOCK_SIZE, CACHE_BLOCK_SIZE);
        bcache_hash_insert((i32)(slot - block_cache));
      }
      bcache_release(flags);
      if (ra)
        kfree(ra);
      break;
    }
  }
  return 0;
}

/* Variant D — dirty throttle + faster writeback. The in-guest build streams .o
 * output to disk; each 512-byte block is marked BLK_CACHE_DIRTY and, before, was
 * written back ONE block per device command (slow polled AHCI) only when its
 * slot got recycled. Under memory pressure that bunched all the writeback at the
 * worst moment and crawled. bcache_flush_some() instead proactively drains dirty
 * blocks in CONTIGUOUS runs — one write_blocks() command per run, amortising the
 * per-command cost — and blk_write_cached calls it every N dirty writes so dirty
 * blocks never pile up unbounded (the writer is throttled by doing some of the
 * writeback itself, like Linux balance_dirty_pages). */
/* Ceiling on the sectors coalesced into one writeback command (512 = 256 KiB,
 * the same order as a Linux writeback chunk). The actual run is this clamped to
 * the device's own max_sectors, so a device that cannot describe 256 KiB in one
 * command never gets asked to. The ceiling itself stays here because the index
 * array below lives on the kernel stack: 512 entries is 2 KiB, and a stack
 * buffer is the one thing that must not grow with a device's appetite. */
#define BCACHE_FLUSH_RUN     512
/* Dirty writes between proactive drains — a FRACTION of the pool, not a fixed
 * count. 256 was chosen against an 8192-entry cache (3% of it); left constant
 * after the pool grew to 65536 it throttles the writer eight times more often
 * than intended, and the writer pays that cost inline. Measured on a browser
 * start, the main thread sat in bcache_flush_some issuing device commands
 * instead of making progress. */
/* Dirty blocks written between proactive drains.
 *
 * A thirty-second of the pool was fine while every fsync drained the whole
 * cache. Now that fsync only persists its own file, nothing else pushes the
 * rest out — and once the pool is entirely dirty, every read miss has to write
 * a block back before it can take a slot, so reads start paying for writes. A
 * finer slice keeps the background drain ahead of that. */
#define BCACHE_DIRTY_THROTTLE (block_cache_n / 128u)

static volatile int bcache_flushing; /* re-entrancy guard (kmalloc may reclaim) */

static usize bcache_flush_some(usize target) {
  if (target == 0)
    return 0;
  if (__sync_lock_test_and_set(&bcache_flushing, 1))
    return 0; /* another flush in progress — don't recurse/contend */

  usize flushed = 0;
  u64 flags = bcache_acquire();
  while (flushed < target) {
    /* Find a dirty, non-busy, writable block to anchor a run. */
    int start = -1;
    for (usize i = 0; i < block_cache_n; i++) {
      struct block_buffer *e = &block_cache[i];
      if ((e->flags & (BLK_CACHE_VALID | BLK_CACHE_DIRTY)) ==
              (BLK_CACHE_VALID | BLK_CACHE_DIRTY) &&
          !(e->flags & BLK_CACHE_BUSY) && e->bdev && e->bdev->write_blocks) {
        start = (int)i;
        break;
      }
    }
    if (start < 0)
      break; /* nothing dirty to flush */

    struct block_device *wdev = block_cache[start].bdev;
    u64 base = block_cache[start].block_no;
    /* Extend a contiguous run [base, base+run) of dirty, non-busy blocks on the
     * same device; claim each BUSY so a concurrent write to those LBAs waits. */
    i32 idxs[BCACHE_FLUSH_RUN];
    usize run_max = blk_max_sectors(wdev);
    if (run_max > BCACHE_FLUSH_RUN)
      run_max = BCACHE_FLUSH_RUN;
    usize run = 0;
    for (u64 l = base; run < run_max; l++) {
      struct block_buffer *e = bcache_find(wdev, l);
      if (!e || !(e->flags & BLK_CACHE_DIRTY) || (e->flags & BLK_CACHE_BUSY))
        break;
      e->flags |= BLK_CACHE_BUSY;
      idxs[run++] = (i32)(e - block_cache);
    }
    if (run == 0)
      break;

    u8 *tmp = (u8 *)kmalloc(run * CACHE_BLOCK_SIZE);
    if (!tmp) {
      /* No memory to coalesce — unbusy and bail (eviction-time writeback still
       * handles these later). */
      for (usize k = 0; k < run; k++)
        block_cache[idxs[k]].flags &= ~BLK_CACHE_BUSY;
      break;
    }
    for (usize k = 0; k < run; k++)
      memcpy(tmp + k * CACHE_BLOCK_SIZE, block_cache[idxs[k]].data,
             CACHE_BLOCK_SIZE);

    bcache_release(flags); /* write_blocks may yield — lock dropped, slots BUSY */
    int wr = blk_dev_write(wdev, base, (u32)run, tmp);
    kfree(tmp);
    flags = bcache_acquire();

    for (usize k = 0; k < run; k++) {
      struct block_buffer *e = &block_cache[idxs[k]];
      e->flags &= ~BLK_CACHE_BUSY;
      if (wr == 0)
        e->flags &= ~BLK_CACHE_DIRTY; /* persisted; keep DIRTY on failure (R3-13) */
    }
    if (wr != 0)
      break; /* device error — stop, leave the rest dirty for a later retry */
    flushed += run;
  }
  bcache_release(flags);
  __sync_lock_release(&bcache_flushing);
  return flushed;
}

int blk_write_cached(struct block_device *dev, u64 lba, u32 count,
                     const void *buffer) {
  if (!dev || !dev->write_blocks)
    return -1;
  if (dev->block_count > 0 && (count > dev->block_count || lba > dev->block_count - count)) {
    return -1;
  }
  if (dev->block_size != CACHE_BLOCK_SIZE) {
    return blk_dev_write(dev, lba, count, buffer);
  }
  dev = blk_cache_target(dev, &lba);

  const u8 *buf8 = (const u8 *)buffer;
  for (u32 i = 0; i < count; i++) {
    u64 current_lba = lba + i;
    for (;;) {
      u64 flags = bcache_acquire();
      struct block_buffer *entry = bcache_find(dev, current_lba);

      /* SMP: a found entry may be mid-eviction — bcache_evict marks the slot
       * BLK_CACHE_BUSY and drops bcache_lock while a dirty write-back DMA
       * reads entry->data on another CPU. Writing into it now would (a) tear
       * the in-flight DMA and (b) be silently dropped when the evictor clears
       * BLK_CACHE_DIRTY after the write-back. Wait for the write-back to land,
       * then retry. (The read path is safe: it only reads entry->data, so it
       * may share the buffer with the write-back DMA.) */
      if (entry && (entry->flags & BLK_CACHE_BUSY)) {
        bcache_release(flags);
        scheduler_yield();
        continue;
      }

      if (!entry) {
        entry = bcache_evict(&flags);
        if (!entry) { /* all slots in-flight on other CPUs — retry */
          bcache_release(flags);
          scheduler_yield();
          continue;
        }
        /* bcache_evict may have dropped the lock for a write-back; re-check
         * for a concurrently-filled entry to avoid a duplicate (dev,lba). */
        struct block_buffer *raced = bcache_find(dev, current_lba);
        if (raced) {
          if (raced->flags & BLK_CACHE_BUSY) {
            bcache_release(flags);
            scheduler_yield();
            continue;
          }
          entry = raced; /* our evicted slot stays free; write into the winner */
        } else {
          entry->bdev = dev;
          entry->block_no = current_lba;
          entry->flags |= BLK_CACHE_VALID;
          entry->flags &= ~BLK_CACHE_BUSY;
          entry->last_used = ++bcache_tick;
          bcache_hash_insert((i32)(entry - block_cache));
        }
      }

      /* The whole fill happens under the lock (no yielding DMA), so no BUSY
       * claim is needed here — unlike the read path. */
      memcpy(entry->data, buf8 + i * CACHE_BLOCK_SIZE, CACHE_BLOCK_SIZE);
      entry->flags |= BLK_CACHE_DIRTY; /* write-back: flush later */
      {
        unsigned os = blk_owner_slot();

        entry->dirty_fsid = blk_dirty_owner[os].fsid;
        entry->dirty_ino = blk_dirty_owner[os].ino;
      }
      bcache_release(flags);
      break;
    }
  }
  /* Variant D throttle: every BCACHE_DIRTY_THROTTLE dirtied blocks, proactively
   * drain a larger batch in contiguous runs so dirty blocks never accumulate to
   * fill the cache (which would force a slow per-block writeback on every later
   * eviction). The writer pays some writeback cost here — that IS the throttle.
   * Counter is a heuristic; an SMP race only shifts the drain cadence. */
  static u32 dirty_writes;
  u32 throttle = BCACHE_DIRTY_THROTTLE;
  if (throttle < 256u)
    throttle = 256u; /* tiny guests keep the original cadence */
  dirty_writes += count;
  if (dirty_writes >= throttle) {
    dirty_writes = 0;
    bcache_flush_some(throttle * 2u);
  }
  return 0;
}

/* POSIX: Fsync/Sync support - Flush all dirty blocks to physical storage */
void blk_flush_buffer(struct block_buffer *buf) {
  if (!buf || !(buf->flags & BLK_CACHE_DIRTY))
    return;

  if (buf->bdev && buf->bdev->write_blocks) {
    /* Only clear DIRTY when the device actually accepted the write. Clearing it
     * unconditionally on a failed write silently loses the data and lets
     * blk_sync_all()/umount report success (R3-13). Leave it dirty for retry. */
    if (buf->bdev->write_blocks(buf->bdev, buf->block_no, 1, buf->data) == 0)
      buf->flags &= ~BLK_CACHE_DIRTY;
  }
}

/* Forget every cached copy of [lba, lba+count) on `target`. Used before a
 * command that changes the medium behind the cache's back (WRITE ZEROES,
 * DISCARD): a stale entry that is still dirty would otherwise be written back
 * over the new contents later, and a stale clean one would keep answering
 * reads with the old contents. Entries mid-DMA are waited out, not stolen. */
static void bcache_drop_range(struct block_device *target, u64 lba, u32 count) {
  for (usize i = 0; i < block_cache_n; i++) {
    for (;;) {
      u64 flags = bcache_acquire();
      struct block_buffer *e = &block_cache[i];
      if (!(e->flags & BLK_CACHE_VALID) || e->bdev != target ||
          e->block_no < lba || e->block_no >= lba + count) {
        bcache_release(flags);
        break;
      }
      if (e->flags & BLK_CACHE_BUSY) {
        /* Another CPU is DMA-ing into this slot; let it finish. */
        bcache_release(flags);
        scheduler_yield();
        continue;
      }
      bcache_hash_remove((i32)i);
      e->flags = 0;
      e->bdev = 0;
      e->block_no = 0;
      bcache_release(flags);
      break;
    }
  }
}

/* One page of zeroes, shared by every fallback in blk_zero_blocks(). Zeroing a
 * block used to mean a kmalloc + memset per allocated filesystem block. */
static const u8 blk_zero_page[4096];

int blk_zero_blocks(struct block_device *dev, u64 lba, u32 count) {
  if (!dev || count == 0)
    return -1;
  if (dev->block_count > 0 &&
      ((u64)count > dev->block_count || lba > dev->block_count - count))
    return -1;

  if (dev->block_size == CACHE_BLOCK_SIZE) {
    u64 tlba = lba;
    struct block_device *target = blk_cache_target(dev, &tlba);
    if (target->write_zeroes) {
      bcache_drop_range(target, tlba, count);
      if (target->write_zeroes(target, tlba, count) == 0)
        return 0;
      /* The device refused it — write the zeroes ourselves rather than leave
       * the caller believing the range was cleared. */
    }
  }

  u32 done = 0;
  while (done < count) {
    u32 chunk = count - done;
    if (chunk > sizeof(blk_zero_page) / CACHE_BLOCK_SIZE)
      chunk = sizeof(blk_zero_page) / CACHE_BLOCK_SIZE;
    if (blk_write_cached(dev, lba + done, chunk, blk_zero_page) < 0)
      return -1;
    done += chunk;
  }
  return 0;
}

/* M109 discard (blkdiscard, fstrim). Tell the device the range no longer holds
 * anything worth keeping. Unlike blk_zero_blocks() there is NO fallback: a
 * discard is a hint, and emulating it by writing zeroes would turn a hint into
 * the very I/O it exists to avoid — and would lie about the resulting content,
 * which after a real discard is whatever the device chooses to return. A
 * device with no discard command therefore reports -EOPNOTSUPP and the caller
 * says so, exactly as Linux does on a disk that cannot trim. */
int blk_discard_blocks(struct block_device *dev, u64 lba, u32 count) {
  if (!dev || count == 0)
    return -EINVAL;
  if (dev->block_count > 0 &&
      ((u64)count > dev->block_count || lba > dev->block_count - count))
    return -EINVAL;
  if (dev->block_size != CACHE_BLOCK_SIZE)
    return -EOPNOTSUPP;

  u64 tlba = lba;
  struct block_device *target = blk_cache_target(dev, &tlba);
  if (!target->discard)
    return -EOPNOTSUPP;

  bcache_drop_range(target, tlba, count);
  return target->discard(target, tlba, count) == 0 ? 0 : -EIO;
}

int blk_discard_supported(struct block_device *dev) {
  if (!dev || dev->block_size != CACHE_BLOCK_SIZE)
    return 0;
  u64 tlba = 0;
  struct block_device *target = blk_cache_target(dev, &tlba);
  return target->discard != 0;
}

/* Write a run of consecutive dirty blocks with one device call.
 *
 * The cache holds each block in its own buffer, and the device takes one
 * contiguous region — so a run is copied into a scratch buffer first. That
 * copy is memory bandwidth measured in microseconds; the round trip it saves
 * is a queue notification, a device turnaround and, when the driver's short
 * spin misses, a whole scheduler tick. The scratch buffer is allocated once
 * per flush and only when a run longer than one block actually turns up.
 *
 * DIRTY is cleared only for blocks the device accepted, exactly as the
 * single-block path does: a failed write that cleared it would lose the data
 * and let the next sync report success.
 */
static void blk_flush_run(struct block_buffer **run, usize n, u8 **bounce) {
  if (n == 0)
    return;
  if (n == 1) {
    blk_flush_buffer(run[0]);
    return;
  }

  struct block_device *dev = run[0]->bdev;

  if (!dev || !dev->write_blocks)
    return;
  if (!*bounce) {
    /* Sized to the longest run the flusher will ever assemble, which is the
     * same device-derived bound. */
    *bounce = (u8 *)kmalloc(BCACHE_FLUSH_RUN * sizeof(run[0]->data));
    if (!*bounce) {
      for (usize i = 0; i < n; i++)
        blk_flush_buffer(run[i]);
      return;
    }
  }
  for (usize i = 0; i < n; i++)
    memcpy(*bounce + i * sizeof(run[i]->data), run[i]->data,
           sizeof(run[i]->data));
  if (dev->write_blocks(dev, run[0]->block_no, (u32)n, *bounce) == 0) {
    for (usize i = 0; i < n; i++)
      run[i]->flags &= ~BLK_CACHE_DIRTY;
  } else {
    for (usize i = 0; i < n; i++)
      blk_flush_buffer(run[i]);
  }
}

/* Put a collected set of dirty buffers in block order and write it in runs. */
static void blk_flush_sorted(struct block_buffer **set, usize n, u8 **bounce) {
  if (n == 0)
    return;

  /* Insertion sort: the sets are small (a few hundred at most, and a file's
   * worth is typically far less) and very nearly ordered already, since the
   * cache tends to hold a file's blocks in the order they were allocated. */
  for (usize i = 1; i < n; i++) {
    struct block_buffer *v = set[i];
    usize j = i;

    /* By device first, then by block: the whole-cache drain hands this a set
     * spanning every device, and a run may only ever be one device's. */
    while (j > 0 && (set[j - 1]->bdev > v->bdev ||
                     (set[j - 1]->bdev == v->bdev &&
                      set[j - 1]->block_no > v->block_no))) {
      set[j] = set[j - 1];
      j--;
    }
    set[j] = v;
  }

  usize start = 0;
  for (usize i = 1; i <= n; i++) {
    /* How long a run may be is the device's business: the AHCI PRDT, the
     * virtio descriptor chain and the NVMe PRP list all describe a different
     * maximum, and a constant that fits the smallest wastes the others. */
    usize run_cap = blk_max_sectors(set[start]->bdev);

    if (run_cap == 0 || run_cap > BCACHE_FLUSH_RUN)
      run_cap = BCACHE_FLUSH_RUN;
    int breaks = (i == n) || set[i]->bdev != set[i - 1]->bdev ||
                 set[i]->block_no != set[i - 1]->block_no + 1 ||
                 (i - start) >= run_cap;

    if (!breaks)
      continue;
    blk_flush_run(&set[start], i - start, bounce);
    start = i;
  }
}

/* How long a filesystem may hold a metadata change in memory before it must
 * reach the disk.
 *
 * The counterpart of Linux's dirty_expire_centisecs, and the reason it lives
 * here rather than in a filesystem: the deadline belongs to the block layer's
 * writeback policy, and a filesystem that invents its own drifts away from it.
 * Half a second — long enough that a burst of allocations rewrites the
 * superblock once instead of once per block, short enough that a crash loses
 * only counts a check would recompute anyway. */
u64 blk_writeback_interval_ticks(void) { return SCHED_TICKS_PER_SEC / 2; }

/* Write every dirty cache entry back into whatever device owns it.
 *
 * In runs, like the other two flush paths: this is the periodic drain, so it
 * is where most of a write-heavy workload's blocks actually reach the disk,
 * and one device command per 512-byte block is what made a run that wrote six
 * megabytes of files issue a quarter of a million requests. */
static void bcache_drain_all(void) {
  enum { SCAN_CHUNK = 256 };
  usize cap = 512;
  struct block_buffer **set = (struct block_buffer **)kmalloc(cap * sizeof(*set));
  usize n = 0;
  u8 *bounce = 0;

  for (usize base = 0; base < block_cache_n; base += SCAN_CHUNK) {
    usize stop = base + SCAN_CHUNK;

    if (stop > block_cache_n)
      stop = block_cache_n;

    u64 flags = bcache_acquire();
    for (usize i = base; i < stop; i++) {
      struct block_buffer *b = &block_cache[i];

      if (!((b->flags & BLK_CACHE_VALID) && (b->flags & BLK_CACHE_DIRTY)))
        continue;
      if (set && n < cap)
        set[n++] = b;
      else if (!set)
        blk_flush_buffer(b);
    }
    bcache_release(flags);
    if (set && n == cap) {
      blk_flush_sorted(set, n, &bounce);
      n = 0;
    }
  }
  if (set) {
    blk_flush_sorted(set, n, &bounce);
    kfree(set);
  }
  if (bounce)
    kfree(bounce);
}

/* Flush every whole disk we know of. Partitions are skipped: they share their
 * parent's medium, and the parent is registered too, so flushing both would
 * only cost a second command. */
static void blk_flush_all_devices(void) {
  for (usize i = 0; i < blk_device_count; i++) {
    struct block_device *dev = blk_devices[i];
    if (dev && dev->flush && !blk_is_partition(dev))
      dev->flush(dev);
  }
}

void blk_sync_all(void) {
  /* Two rounds, because one device can sit on top of another. A loop device's
   * flush writes its backing file, and those writes land in the block cache of
   * the disk that file lives on — after the first drain has already passed it.
   * The second round is what actually gets those bytes onto that disk. Nothing
   * stacks on a loop device in turn, so two rounds are enough; on a system with
   * no stacked device the second round finds nothing dirty and costs one extra
   * (cheap) flush command per disk. */
  bcache_drain_all();
  blk_flush_all_devices();
  bcache_drain_all();
  blk_flush_all_devices();
}

/* Write back the dirty blocks this file owns, then flush the device.
 *
 * fsync on Linux persists one file, not the disk. Here it drained every dirty
 * block in a cache sized at a fraction of RAM, which on a browser's profile
 * cost seconds per call — 38 calls, 4.6 s apiece, most of it other files'
 * data. Blocks carry the owner that dirtied them now, so this writes those and
 * leaves the rest to the background drain. Unstamped blocks are written too:
 * they predate the stamping or came from a path that does not set an owner,
 * and it is better to write a stranger's block than to lose this one's. */
/* Write back every dirty block of `dev` in [first, last), optionally only
 * those a given inode dirtied, in block order and in runs.
 *
 * Shared by the per-file flush and the whole-device drain, because both used to
 * take and release the cache lock once per cache entry — sixty thousand of them
 * on an ordinary guest — and then hand each dirty block to the device on its
 * own. A profile found the unlock third among all kernel costs and the port
 * write that notifies the device first, at 29%: the cache's block is 512 bytes,
 * so a 4 KiB page written back one block at a time is eight notifications,
 * eight completions and eight interrupts.
 *
 * Sorting is what makes the runs real. The cache is scanned in slot order and a
 * file's consecutive blocks are scattered across slots, so joining neighbours
 * as they turn up yields runs of one almost every time — measured, and worth
 * nothing. In block order a sequentially written file collapses to a handful of
 * runs.
 */
static void blk_flush_matching(struct block_device *dev, u64 first, u64 last,
                               u32 fsid, u64 ino) {
  enum { SCAN_CHUNK = 256 };
  usize cap = 512;
  struct block_buffer **set = (struct block_buffer **)kmalloc(cap * sizeof(*set));
  usize n = 0;
  u8 *bounce = 0;

  for (usize base = 0; base < block_cache_n; base += SCAN_CHUNK) {
    usize stop = base + SCAN_CHUNK;

    if (stop > block_cache_n)
      stop = block_cache_n;

    u64 flags = bcache_acquire();
    for (usize i = base; i < stop; i++) {
      struct block_buffer *b = &block_cache[i];

      if (!((b->flags & BLK_CACHE_VALID) && b->bdev == dev &&
            b->block_no >= first && b->block_no < last &&
            (b->flags & BLK_CACHE_DIRTY)))
        continue;
      /* ino == 0 means "every dirty block of this device". */
      if (ino && !(b->dirty_ino == 0 ||
                   (b->dirty_ino == ino && b->dirty_fsid == fsid)))
        continue;
      if (set && n < cap)
        set[n++] = b;
      else if (!set)
        blk_flush_buffer(b); /* no memory for the set: the old behaviour */
    }
    bcache_release(flags);
    if (set && n == cap) {
      blk_flush_sorted(set, n, &bounce);
      n = 0;
    }
  }
  if (set) {
    blk_flush_sorted(set, n, &bounce);
    kfree(set);
  }
  if (bounce)
    kfree(bounce);
}

int blk_cache_flush_inode(struct block_device *dev, u32 fsid, u64 ino) {
  if (!dev)
    return -1;
  if (!dev->write_blocks)
    return 0;

  u64 first = 0;
  u64 last = ~0ULL;

  if (blk_is_partition(dev)) {
    struct partition_device *part = (struct partition_device *)dev->priv;

    if (!part || !part->parent)
      return -1;
    first = part->start_lba;
    last = part->start_lba + dev->block_count;
    dev = part->parent;
  }

  blk_flush_matching(dev, first, last, fsid, ino);

  if (dev->flush)
    dev->flush(dev);
  return 0;
}

void blk_cache_flush(struct block_device *dev) {
  if (!dev) {
    blk_sync_all();
    return;
  }
  if (!dev->write_blocks)
    return;

  /* Entries are keyed by the parent (see blk_cache_target), so flushing a
   * partition means flushing the parent's entries that fall inside it. */
  u64 first = 0;
  u64 last = ~0ULL;
  if (blk_is_partition(dev)) {
    struct partition_device *part = (struct partition_device *)dev->priv;
    if (!part || !part->parent)
      return;
    first = part->start_lba;
    last = part->start_lba + dev->block_count;
    dev = part->parent;
  }

  /* Same collect-sort-write as the per-file flush: the cache holds 512-byte
   * blocks, so a device drained one block at a time turns every 4 KiB of a
   * file into eight round trips. Measured on a write workload: 240,000 device
   * requests for 177 MiB, an average of 738 bytes each. */
  blk_flush_matching(dev, first, last, 0, 0);

  /* `dev` is the parent by now; the drain above only got the bytes as far as
   * the medium's own write-back cache, so finish the job. */
  if (dev->flush)
    dev->flush(dev);
}

/* M14: prove that the durability path is real.
 *
 * Two things are checked, and neither of them can pass by accident:
 *  1. Every whole disk that claims a cache-flush command accepts one. This is
 *     the ATA FLUSH CACHE EXT / NVMe FLUSH / virtio FLUSH that used to be
 *     absent from fsync(2) altogether (AHCI issued one after every write
 *     instead, which is a different thing entirely, and NVMe issued none).
 *  2. On a scratch virtio disk — the only device here that nothing else owns —
 *     a written pattern survives a flush plus a full cache drop, and
 *     blk_zero_blocks() really leaves zeroes behind, whether the device did
 *     the zeroing itself or the block layer wrote them.
 * Reads go through blk_cache_invalidate() first, so a pass means the bytes came
 * back from the medium, not from the cache entry the write left behind. */
#define BLK_SELFTEST_LBA 2048
#define BLK_SELFTEST_BLOCKS 8

void blk_durability_selftest(void) {
  for (usize i = 0; i < blk_device_count; i++) {
    struct block_device *dev = blk_devices[i];
    if (!dev || !dev->flush || blk_is_partition(dev))
      continue;
    int rc = dev->flush(dev);
    console_write(rc == 0 ? "M14-BLK: ok flush-op dev=" : "M14-BLK: FAIL flush-op dev=");
    console_write(dev->name ? dev->name : "?");
    console_write("\n");
  }

  struct block_device *dev = blk_nth_on_bus(BLK_BUS_VIRTIO, 0);
  if (!dev || !dev->write_blocks) {
    k_info(NULL, "M14-BLK: no virtio-blk scratch device");
    return;
  }

  const u32 nbytes = BLK_SELFTEST_BLOCKS * CACHE_BLOCK_SIZE;
  u8 *out = kmalloc(nbytes);
  u8 *in = kmalloc(nbytes);
  if (!out || !in) {
    if (out) kfree(out);
    if (in) kfree(in);
    k_info(NULL, "M14-BLK: FAIL durable-roundtrip out-of-memory");
    return;
  }
  for (u32 b = 0; b < nbytes; b++)
    out[b] = (u8)(0xA5 ^ (b * 7));

  int ok = blk_write_cached(dev, BLK_SELFTEST_LBA, BLK_SELFTEST_BLOCKS, out) >= 0;
  if (ok) {
    /* Drains the dirty entries into the driver, then flushes the device. */
    blk_cache_flush(dev);
    blk_cache_invalidate(dev);
    ok = blk_read_cached(dev, BLK_SELFTEST_LBA, BLK_SELFTEST_BLOCKS, in) >= 0;
  }
  if (ok) {
    for (u32 b = 0; b < nbytes; b++) {
      if (in[b] != out[b]) {
        ok = 0;
        break;
      }
    }
  }
  console_write(ok ? "M14-BLK: ok durable-roundtrip dev=" : "M14-BLK: FAIL durable-roundtrip dev=");
  console_write(dev->name ? dev->name : "?");
  console_write("\n");

  int used_device_op = dev->write_zeroes != 0;
  int zok = blk_zero_blocks(dev, BLK_SELFTEST_LBA, BLK_SELFTEST_BLOCKS) == 0;
  if (zok) {
    blk_cache_invalidate(dev);
    memset(in, 0xFF, nbytes);
    zok = blk_read_cached(dev, BLK_SELFTEST_LBA, BLK_SELFTEST_BLOCKS, in) >= 0;
  }
  if (zok) {
    for (u32 b = 0; b < nbytes; b++) {
      if (in[b] != 0) {
        zok = 0;
        break;
      }
    }
  }
  console_write(zok ? "M14-BLK: ok zero-blocks by=" : "M14-BLK: FAIL zero-blocks by=");
  console_write(used_device_op ? "device" : "blocklayer");
  console_write("\n");

  kfree(out);
  kfree(in);
}

void blk_cache_invalidate(struct block_device *dev) {
  if (!dev) return;
  u64 first = 0;
  u64 last = ~0ULL;
  if (blk_is_partition(dev)) {
    struct partition_device *part = (struct partition_device *)dev->priv;
    if (!part || !part->parent)
      return;
    first = part->start_lba;
    last = part->start_lba + dev->block_count;
    dev = part->parent;
  }
  for (usize i = 0; i < block_cache_n; i++) {
    u64 flags = bcache_acquire();
    if (block_cache[i].bdev == dev && block_cache[i].block_no >= first &&
        block_cache[i].block_no < last) {
      if ((block_cache[i].flags & BLK_CACHE_VALID) && (block_cache[i].flags & BLK_CACHE_DIRTY)) {
        struct block_buffer *buf = &block_cache[i];
        bcache_release(flags);
        blk_flush_buffer(buf);
        flags = bcache_acquire();
      }
      /* Unhash before zeroing — otherwise the chain head ends up pointing
       * at a slot whose (bdev, block_no) keys are 0, polluting future
       * lookups for any read at LBA 0 on dev==0. */
      bcache_hash_remove((i32)i);
      block_cache[i].flags = 0;
      block_cache[i].bdev = 0;
      block_cache[i].block_no = 0;
    }
    bcache_release(flags);
  }
}

/* ── /dev block-device nodes (for BusyBox blkid/fdisk) ──
 * Each registered block device gets a /dev/<name> node whose byte-addressed
 * reads/writes translate to cached block I/O. Sub-block writes do a
 * read-modify-write so fdisk can rewrite just the MBR. */
/* Blocks per driver command on the raw-device bulk fast path. This used to be
 * one constant (1536, chosen to stay inside AHCI's 248-entry PRDT) applied to
 * every device; it is now the device's own max_sectors, so an NVMe namespace
 * whose MDTS allows 2 MiB is no longer chopped to the size an AHCI port can
 * describe. Larger = fewer commands = fewer per-command completion waits, which
 * dominate raw-disk throughput. */

static isize blkdev_node_read(struct vfs_node *node, u64 offset, char *buffer,
                              usize size, int flags) {
  (void)flags;
  struct block_device *dev = node->inode->blk_dev;
  if (!dev)
    return -EIO;
  usize bs = dev->block_size ? dev->block_size : 512;
  u64 total = (u64)bs * dev->block_count;
  if (offset >= total)
    return 0;
  if (size > total - offset)
    size = (usize)(total - offset);
  if (size == 0)
    return 0;
  u64 first = offset / bs;
  u64 last = (offset + size - 1) / bs;
  u32 nblk = (u32)(last - first + 1);
  /* Fast path: a block-aligned bulk read (e.g. a disk imager's 1 MiB reads of
   * /dev/sdX) goes straight to the driver as one DMA, skipping the per-512B
   * blk_read_cached loop (lock+tiny-DMA+memcpy per block ~ 100x slower for
   * large raw reads). Raw whole-disk reads bypass the page cache as on Unix. */
  if (dev->read_blocks && (offset % bs) == 0 && (size % bs) == 0) {
    u8 *bulk = kmalloc((usize)nblk * bs);   /* DMA needs a kernel buffer */
    if (bulk) {
      /* Issue the bulk DMA in driver-safe sub-chunks: a single AHCI command's
       * PRDT (NVMe PRP / virtio SG likewise) is bounded, so one giant transfer
       * would overflow the per-page descriptor table. The device's own
       * max_sectors keeps each command well within that, while still being
       * ~hundreds× fewer, far larger transfers than the per-512B cache loop. */
      int ok = 1;
      const u32 bulk_max = blk_max_sectors(dev);
      for (u32 done = 0; done < nblk; ) {
        u32 chunk = nblk - done;
        if (chunk > bulk_max) chunk = bulk_max;
        /* read_blocks returns the sector count (>=0) on success, <0 on error. */
        if (blk_dev_read(dev, first + done, chunk,
                             bulk + (usize)done * bs) < 0) { ok = 0; break; }
        done += chunk;
      }
      if (ok) memcpy(buffer, bulk, size);
      kfree(bulk);
      if (ok) return (isize)size;
      return -EIO;
    }
    /* OOM on the bulk buffer → fall through to the cached per-block path. */
  }
  u8 *tmp = kmalloc((usize)nblk * bs);
  if (!tmp)
    return -ENOMEM;
  if (blk_read_cached(dev, first, nblk, tmp) != 0) {
    kfree(tmp);
    return -EIO;
  }
  memcpy(buffer, tmp + (usize)(offset - first * bs), size);
  kfree(tmp);
  return (isize)size;
}

static isize blkdev_node_write(struct vfs_node *node, u64 offset,
                               const char *buffer, usize size, int flags) {
  (void)flags;
  struct block_device *dev = node->inode->blk_dev;
  if (!dev)
    return -EIO;
  usize bs = dev->block_size ? dev->block_size : 512;
  u64 total = (u64)bs * dev->block_count;
  if (offset >= total)
    return 0;
  if (size > total - offset)
    size = (usize)(total - offset);
  if (size == 0)
    return 0;
  u64 first = offset / bs;
  u64 last = (offset + size - 1) / bs;
  u32 nblk = (u32)(last - first + 1);
  /* Fast path: a block-aligned bulk write (disk imager's 1 MiB writes) DMAs
   * straight to the driver in one shot, then drops any cached copies of those
   * blocks so later cached reads re-fetch from disk (invalidate flushes dirty
   * first, so no data loss). Skips the read-modify-write + per-block cache loop. */
  if (dev->write_blocks && (offset % bs) == 0 && (size % bs) == 0) {
    u8 *bulk = kmalloc((usize)nblk * bs);   /* DMA needs a kernel buffer */
    if (bulk) {
      memcpy(bulk, buffer, size);
      /* Drop any cached copies of these blocks FIRST (flushing existing dirty
       * entries to disk), so the direct write below is the authoritative final
       * state. Doing it AFTER would flush stale dirty entries (e.g. a swap
       * header on an auto-claimed disk) over the data we just wrote. */
      blk_cache_invalidate(dev);
      int ok = 1;
      const u32 bulk_max = blk_max_sectors(dev);
      for (u32 done = 0; done < nblk; ) {   /* driver-safe sub-chunks (PRDT) */
        u32 chunk = nblk - done;
        if (chunk > bulk_max) chunk = bulk_max;
        /* write_blocks returns the sector count (>=0) on success, <0 on error. */
        if (blk_dev_write(dev, first + done, chunk,
                              bulk + (usize)done * bs) < 0) { ok = 0; break; }
        done += chunk;
      }
      kfree(bulk);
      if (ok) return (isize)size;
      return -EIO;
    }
    /* OOM on the bulk buffer → fall through to the cached read-modify path. */
  }
  u8 *tmp = kmalloc((usize)nblk * bs);
  if (!tmp)
    return -ENOMEM;
  if (blk_read_cached(dev, first, nblk, tmp) != 0) {
    kfree(tmp);
    return -EIO;
  }
  memcpy(tmp + (usize)(offset - first * bs), buffer, size);
  if (blk_write_cached(dev, first, nblk, tmp) != 0) {
    kfree(tmp);
    return -EIO;
  }
  kfree(tmp);
  return (isize)size;
}

/* Make one VFS node behave as this block device: byte reads and writes routed
 * through the cached block layer, the size stat() reports, and the device
 * number userspace identifies it by. Shared by the boot-time node creation
 * below and by mknod(2), so a node mdev creates from a uevent is wired exactly
 * like one the kernel made itself. */
static void blk_wire_dev_node(struct vfs_node *node, struct block_device *dev,
                              usize index) {
  node->inode->blk_dev = dev;
  node->inode->read_cb = blkdev_node_read;
  node->inode->write_cb = blkdev_node_write;
  node->inode->size = (usize)(dev->block_size * dev->block_count);
  /* The same major:minor /sys/dev/block and /proc/partitions publish. Without
   * it st_rdev was 0 on every block node, so a tool that identifies a device
   * by its number rather than its path — mdev deciding whether the node it
   * finds is the one the event named — had nothing to compare. */
  node->inode->rdev = ((u64)BLK_SYSFS_MAJOR << 8) | (u64)index;
}

struct block_device *blk_from_devno(u64 rdev) {
  if ((rdev >> 8) != BLK_SYSFS_MAJOR)
    return 0;
  return blk_at((usize)(rdev & 0xFF));
}

int blk_bind_dev_node(struct vfs_node *node, u64 rdev) {
  if (!node || !node->inode)
    return -EINVAL;
  usize index = (usize)(rdev & 0xFF);
  struct block_device *dev = blk_from_devno(rdev);
  if (!dev)
    return -ENODEV;
  blk_wire_dev_node(node, dev, index);
  return 0;
}

void blk_create_dev_nodes(void) {
  usize n = blk_count();
  for (usize i = 0; i < n; i++) {
    struct block_device *dev = blk_at(i);
    if (!dev || !dev->name)
      continue;
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", dev->name);
    struct vfs_node *node = vfs_add_node(path, VFS_DEVICE, 0, 0, 0);
    if (!node || IS_ERR(node))
      continue;
    blk_wire_dev_node(node, dev, i);
    node->inode->mode = 0660;
  }
}
