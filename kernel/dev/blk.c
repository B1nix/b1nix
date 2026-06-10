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

#define MAX_BLK_DEVICES 32
#define MAX_BLK_PARTITIONS 32
/* Block-cache sizing (B2 audit): pool capacity is now scaled to actual RAM
 * at blk_cache_init() time so small machines don't waste ~140 KB on a 256-
 * entry pool and big machines aren't starved. ~1 entry per 512 KiB of usable
 * RAM, clamped to [MIN, MAX]. */
#define CACHE_ENTRIES_MIN 64
#define CACHE_ENTRIES_MAX 8192
#define CACHE_BLOCK_SIZE 512

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
  if (partition_count >= MAX_BLK_PARTITIONS)
    return;

  struct partition_device *part = &partitions[partition_count++];
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
  /* O(chain-length) lookup via the (bdev, lba)-keyed hash table. The hash
   * is filled on cache fill / evict so stale (invalid) entries don't linger
   * in chains — but defensively skip any whose flags say invalid. */
  u32 h = bcache_bucket(dev, lba);
  i32 idx = bcache_hash[h];
  while (idx >= 0) {
    struct block_buffer *e = &block_cache[idx];
    if ((e->flags & BLK_CACHE_VALID) && e->bdev == dev && e->block_no == lba) {
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
static struct block_buffer *bcache_evict(u64 *flags_inout) {
  int oldest_idx = -1;
  u32 oldest_tick = 0xFFFFFFFF;

  /* 1. Try to find an invalid (empty), non-busy entry first */
  for (usize i = 0; i < block_cache_n; i++) {
    if (!(block_cache[i].flags & (BLK_CACHE_VALID | BLK_CACHE_BUSY))) {
      return &block_cache[i];
    }
  }

  /* 2. Otherwise, find the LRU (Least Recently Used) non-busy entry */
  for (usize i = 0; i < block_cache_n; i++) {
    if (block_cache[i].flags & BLK_CACHE_BUSY)
      continue;
    if (block_cache[i].last_used < oldest_tick) {
      oldest_tick = block_cache[i].last_used;
      oldest_idx = (int)i;
    }
  }
  if (oldest_idx < 0)
    return 0; /* all entries in-flight — caller yields and retries */

  struct block_buffer *entry = &block_cache[oldest_idx];

  /* 3. Write-Back: flush a dirty entry to disk OUTSIDE the bcache_lock. */
  if ((entry->flags & BLK_CACHE_DIRTY) && entry->bdev && entry->bdev->write_blocks) {
    entry->flags |= BLK_CACHE_BUSY;          /* lock the slot across the drop */
    struct block_device *wb_dev = entry->bdev;
    u64 wb_lba = entry->block_no;
    bcache_release(*flags_inout);            /* RELEASE — write_blocks may yield */
    wb_dev->write_blocks(wb_dev, wb_lba, 1, entry->data);
    *flags_inout = bcache_acquire();         /* REACQUIRE before returning */
    entry->flags &= ~(BLK_CACHE_DIRTY | BLK_CACHE_BUSY);
  }
  /* Unlink from its old hash chain — its (bdev, block_no) is about to be
   * replaced. Leaving it linked would leak chain length and waste lookups. */
  bcache_hash_remove((i32)oldest_idx);
  entry->flags &= ~BLK_CACHE_VALID;
  return entry;
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
    return dev->read_blocks(dev, lba, count, buffer);
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
      entry->bdev = dev;
      entry->block_no = current_lba;
      /* Claim the slot for our lock-free DMA so no other CPU reuses it. */
      entry->flags |= BLK_CACHE_BUSY;
      bcache_release(flags);

      int rc = dev->read_blocks(dev, current_lba, 1, entry->data);
      if (rc < 0) {
        flags = bcache_acquire();
        entry->flags &= ~BLK_CACHE_BUSY; /* release the slot on error */
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
      /* Link this freshly-filled slot into its (bdev, block_no) hash bucket
       * so subsequent bcache_find lookups for the same key are O(chain). */
      bcache_hash_insert((i32)(entry - block_cache));
      memcpy(buf8 + i * CACHE_BLOCK_SIZE, entry->data, CACHE_BLOCK_SIZE);
      bcache_release(flags);
      break;
    }
  }
  return 0;
}

int blk_write_cached(struct block_device *dev, u64 lba, u32 count,
                     const void *buffer) {
  if (!dev || !dev->write_blocks)
    return -1;
  if (dev->block_count > 0 && (count > dev->block_count || lba > dev->block_count - count)) {
    return -1;
  }
  if (dev->block_size != CACHE_BLOCK_SIZE) {
    return dev->write_blocks(dev, lba, count, buffer);
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
        entry->bdev = dev;
        entry->block_no = current_lba;
        entry->flags |= BLK_CACHE_VALID;
        entry->last_used = ++bcache_tick;
        bcache_hash_insert((i32)(entry - block_cache));
      }

      /* The whole fill happens under the lock (no yielding DMA), so no BUSY
       * claim is needed here — unlike the read path. */
      memcpy(entry->data, buf8 + i * CACHE_BLOCK_SIZE, CACHE_BLOCK_SIZE);
      entry->flags |= BLK_CACHE_DIRTY; /* write-back: flush later */
      bcache_release(flags);
      break;
    }
  }
  return 0;
}

/* POSIX: Fsync/Sync support - Flush all dirty blocks to physical storage */
void blk_flush_buffer(struct block_buffer *buf) {
  if (!buf || !(buf->flags & BLK_CACHE_DIRTY))
    return;

  if (buf->bdev && buf->bdev->write_blocks) {
    buf->bdev->write_blocks(buf->bdev, buf->block_no, 1, buf->data);
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
