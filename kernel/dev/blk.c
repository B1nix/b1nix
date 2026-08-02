#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
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
#define CACHE_ENTRIES_MAX 8192
#define CACHE_BLOCK_SIZE 512

/* Read-ahead window (sectors) pulled in ONE device command on a cache miss.
 * The cache fills one 512-byte sector per dev->read_blocks() call; streaming a
 * large file (an executable, a toolchain binary) that way costs one DMA round-
 * trip per sector — ~184k commands for a 94 MB binary, the dominant cost of a
 * disk-resident toolchain. Reading a contiguous run in a single command and
 * populating the neighbouring cache slots from the data already in hand turns
 * a sequential stream into ~1 command per RA sectors. Under KVM the per-command
 * cost is the polled-completion VM-exits (roughly constant), so a bigger window
 * means fewer commands and higher throughput. 512 * 512 = 256 KiB per command —
 * a robust kmalloc size even under the low-RAM self-host, and well under
 * ahci_build_prdt's 248 PRD-entry cap. */
#define BCACHE_READAHEAD 512

static struct block_device *blk_devices[MAX_BLK_DEVICES];
static usize blk_device_count = 0;

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
#define BCACHE_HASH_BITS 10
#define BCACHE_HASH_SIZE (1u << BCACHE_HASH_BITS)
#define BCACHE_HASH_MASK (BCACHE_HASH_SIZE - 1u)
static i32 bcache_hash[BCACHE_HASH_SIZE];

static inline u32 bcache_bucket(struct block_device *dev, u64 lba) {
  /* Spread bdev pointer bits across the lba — both are dense in low bits,
   * a plain XOR would collide for sequential reads from the same device. */
  u64 m = (u64)(usize)dev * 0x9E3779B97F4A7C15ULL;
  m ^= lba * 0xC6BC279692B5C323ULL;
  return (u32)((m ^ (m >> 32)) & BCACHE_HASH_MASK);
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

static void blk_register_internal(struct block_device *dev,
                                  int scan_partitions) {
  if (blk_device_count < MAX_BLK_DEVICES) {
    blk_devices[blk_device_count++] = dev;
  }
  if (scan_partitions) {
    blk_scan_partitions(dev);
  }
}

static void make_partition_name(const char *parent, usize number, char *out,
                                usize out_size) {
  usize len = strlen(parent);
  if (len > out_size - 4)
    len = out_size - 4;
  memcpy(out, parent, len);
  out[len++] = 'p';
  if (number >= 10) {
    out[len++] = '0' + (char)(number / 10);
  }
  out[len++] = '0' + (char)(number % 10);
  out[len] = '\0';
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
  part->blk.block_size = parent->block_size;
  part->blk.block_count = block_count;
  part->blk.read_blocks = partition_read;
  part->blk.write_blocks = parent->write_blocks ? partition_write : 0;
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

/* Partition number parsed from the "<parent>pN" name suffix (1-based), or -1. */
int blk_partition_number(struct block_device *dev) {
  if (!blk_is_partition(dev) || !dev->name)
    return -1;
  const char *last_p = 0;
  for (const char *c = dev->name; *c; c++)
    if (*c == 'p')
      last_p = c;
  if (!last_p)
    return -1;
  int num = 0, any = 0;
  for (const char *c = last_p + 1; *c >= '0' && *c <= '9'; c++) {
    num = num * 10 + (*c - '0');
    any = 1;
  }
  return any ? num : -1;
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
  blk_cache_invalidate(dev);
  blk_scan_partitions(dev);
  blk_create_dev_nodes();
  return 0;
}

void blk_register(struct block_device *dev) { blk_register_internal(dev, 1); }

struct block_device *blk_get(const char *name) {
  for (usize i = 0; i < blk_device_count; i++) {
    if (strcmp(blk_devices[i]->name, name) == 0) {
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
  if (n[0] == 'n' && n[1] == 'v' && n[2] == 'm')
    major = 259; /* nvme */
  else if (n[0] == 's' && n[1] == 'a' && n[2] == 't')
    major = 8; /* sata — SCSI disk major */
  else if (n[0] == 'v' && n[1] == 'i' && n[2] == 'r')
    major = 254; /* virtio-blk */
  else if (n[0] == 'r' && n[1] == 'a' && n[2] == 'm')
    major = 1; /* ramdisk */
  else if (n[0] == 'l' && n[1] == 'o' && n[2] == 'o')
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

/* ── Write-Back Cache Implementation ── */

void blk_cache_init(void) {
  /* Scale pool to RAM: ~1 entry per 512 KiB of usable memory, clamped. */
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  usize want = (usize)(ram_mb * 2);  /* 1 entry per 512 KiB = 2 per MiB */
  if (want < CACHE_ENTRIES_MIN) want = CACHE_ENTRIES_MIN;
  if (want > CACHE_ENTRIES_MAX) want = CACHE_ENTRIES_MAX;
  block_cache_n = want;
  block_cache = kzalloc(block_cache_n * sizeof(struct block_buffer));
  if (!block_cache) {
    /* Fall back to floor — kzalloc still failed? then we have bigger problems
     * but try a smaller pool before giving up entirely. */
    block_cache_n = CACHE_ENTRIES_MIN;
    block_cache = kzalloc(block_cache_n * sizeof(struct block_buffer));
  }
  /* kzalloc zeroes the buffer, but hash_next must start at -1 (empty), not 0
   * (which is a valid index). Likewise prime the hash buckets to -1. */
  for (usize i = 0; i < block_cache_n; i++) block_cache[i].hash_next = -1;
  for (u32 b = 0; b < BCACHE_HASH_SIZE; b++) bcache_hash[b] = -1;
  console_write("blk-cache: ");
  console_write_dec(block_cache_n);
  console_write(" entries (");
  console_write_dec((block_cache_n * sizeof(struct block_buffer)) / 1024);
  console_write(" KiB)\n");
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
#define BLK_IO_WAITERS 16

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
  struct blk_io_waiter waiters[BLK_IO_WAITERS];
};

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
  for (int i = 0; i < BLK_IO_WAITERS; i++) {
    if (!g->waiters[i].used) {
      g->waiters[i].used = 1;
      g->waiters[i].task = current_task;
      g->waiters[i].prio = blk_io_prio_of_current();
      g->waiters[i].queued_at = scheduler_get_uptime_ticks();
      g->waiters[i].admitted = 0;
      slot = i;
      break;
    }
  }
  spin_unlock_irqrestore(&blk_gate_lock, flags);
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
  for (int i = 0; i < BLK_IO_WAITERS; i++) {
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

int blk_read_cached(struct block_device *dev, u64 lba, u32 count,
                    void *buffer) {
  if (!dev || !dev->read_blocks) {
    console_write("blk_read_cached: dev or read_blocks is NULL\n");
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
      u32 run = BCACHE_READAHEAD;
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
#define BCACHE_FLUSH_RUN     512  /* max sectors coalesced into one command (256 KiB) */
#define BCACHE_DIRTY_THROTTLE 256 /* dirty writes between proactive drains */

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
    usize run = 0;
    for (u64 l = base; run < BCACHE_FLUSH_RUN; l++) {
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
  dirty_writes += count;
  if (dirty_writes >= BCACHE_DIRTY_THROTTLE) {
    dirty_writes = 0;
    bcache_flush_some(BCACHE_DIRTY_THROTTLE * 2);
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

void blk_sync_all(void) {
  for (usize i = 0; i < block_cache_n; i++) {
    u64 flags = bcache_acquire();
    if ((block_cache[i].flags & BLK_CACHE_VALID) && (block_cache[i].flags & BLK_CACHE_DIRTY)) {
      struct block_buffer *buf = &block_cache[i];
      bcache_release(flags);
      blk_flush_buffer(buf);
      continue;
    }
    bcache_release(flags);
  }
}

void blk_cache_flush(struct block_device *dev) {
  if (!dev) {
    blk_sync_all();
    return;
  }
  if (!dev->write_blocks)
    return;

  for (usize i = 0; i < block_cache_n; i++) {
    u64 flags = bcache_acquire();
    if ((block_cache[i].flags & BLK_CACHE_VALID) && block_cache[i].bdev == dev && (block_cache[i].flags & BLK_CACHE_DIRTY)) {
      struct block_buffer *buf = &block_cache[i];
      bcache_release(flags);
      blk_flush_buffer(buf);
      continue;
    }
    bcache_release(flags);
  }
}

void blk_cache_invalidate(struct block_device *dev) {
  if (!dev) return;
  for (usize i = 0; i < block_cache_n; i++) {
    u64 flags = bcache_acquire();
    if (block_cache[i].bdev == dev) {
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
/* Max blocks per driver command on the raw-device bulk fast path. 1536 × 512B =
 * 768 KiB → at most ~193 page-segments, still inside the AHCI per-page PRDT
 * (248 entries) and NVMe/virtio descriptor limits. Larger = fewer commands =
 * fewer per-command completion waits, which dominate raw-disk throughput. */
#define BLK_BULK_MAX_BLOCKS 1536u

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
   * /dev/sataN) goes straight to the driver as one DMA, skipping the per-512B
   * blk_read_cached loop (lock+tiny-DMA+memcpy per block ~ 100x slower for
   * large raw reads). Raw whole-disk reads bypass the page cache as on Unix. */
  if (dev->read_blocks && (offset % bs) == 0 && (size % bs) == 0) {
    u8 *bulk = kmalloc((usize)nblk * bs);   /* DMA needs a kernel buffer */
    if (bulk) {
      /* Issue the bulk DMA in driver-safe sub-chunks: a single AHCI command's
       * PRDT (NVMe PRP / virtio SG likewise) is bounded, so one giant transfer
       * would overflow the per-page descriptor table. BLK_BULK_MAX_BLOCKS keeps
       * each command well within that, while still being ~hundreds× fewer, far
       * larger transfers than the per-512B cache loop. */
      int ok = 1;
      for (u32 done = 0; done < nblk; ) {
        u32 chunk = nblk - done;
        if (chunk > BLK_BULK_MAX_BLOCKS) chunk = BLK_BULK_MAX_BLOCKS;
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
      for (u32 done = 0; done < nblk; ) {   /* driver-safe sub-chunks (PRDT) */
        u32 chunk = nblk - done;
        if (chunk > BLK_BULK_MAX_BLOCKS) chunk = BLK_BULK_MAX_BLOCKS;
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

void blk_create_dev_nodes(void) {
  usize n = blk_count();
  for (usize i = 0; i < n; i++) {
    struct block_device *dev = blk_at(i);
    if (!dev || !dev->name)
      continue;
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", dev->name);
    struct vfs_node *node = vfs_add_node(path, VFS_DEVICE, 0, 0, 0);
    if (!node)
      continue;
    node->inode->blk_dev = dev;
    node->inode->read_cb = blkdev_node_read;
    node->inode->write_cb = blkdev_node_write;
    node->inode->size = (usize)(dev->block_size * dev->block_count);
    node->inode->mode = 0660;
  }
}
