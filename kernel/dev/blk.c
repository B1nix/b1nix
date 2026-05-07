#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <stdbool.h>
#include <string.h>

#define MAX_BLK_DEVICES 32
#define MAX_BLK_PARTITIONS 32
#define CACHE_ENTRIES 256
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

static u32 le32(const u8 *p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 le64(const u8 *p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

/* ── Partition I/O ── */

static int partition_read(struct block_device *dev, u64 lba, u32 count,
                          void *buffer) {
  struct partition_device *part = (struct partition_device *)dev->priv;
  if (!part || lba + count > dev->block_count)
    return -1;
  return blk_read_cached(part->parent, part->start_lba + lba, count, buffer);
}

static int partition_write(struct block_device *dev, u64 lba, u32 count,
                           const void *buffer) {
  struct partition_device *part = (struct partition_device *)dev->priv;
  if (!part || lba + count > dev->block_count)
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

/* ── Write-Back Cache Implementation ── */

void blk_cache_init(void) { memset(bcache, 0, sizeof(bcache)); }

static struct blk_cache_entry *bcache_find(struct block_device *dev, u64 lba) {
  for (int i = 0; i < CACHE_ENTRIES; i++) {
    if (bcache[i].valid && bcache[i].dev == dev && bcache[i].lba == lba) {
      bcache[i].last_used = ++bcache_tick;
      return &bcache[i];
    }
  }
  return 0;
}

static struct blk_cache_entry *bcache_evict(void) {
  int oldest_idx = 0;
  u32 oldest_tick = 0xFFFFFFFF;

  /* 1. Try to find an invalid (empty) entry first */
  for (int i = 0; i < CACHE_ENTRIES; i++) {
    if (!bcache[i].valid) {
      return &bcache[i];
    }
  }

  /* 2. Otherwise, find the LRU (Least Recently Used) entry */
  for (int i = 0; i < CACHE_ENTRIES; i++) {
    if (bcache[i].last_used < oldest_tick) {
      oldest_tick = bcache[i].last_used;
      oldest_idx = i;
    }
  }

  struct blk_cache_entry *entry = &bcache[oldest_idx];

  /* 3. Write-Back: If the evicted block is dirty, write it to physical disk */
  if (entry->dirty && entry->dev && entry->dev->write_blocks) {
    entry->dev->write_blocks(entry->dev, entry->lba, 1, entry->data);
    entry->dirty = false;
  }
  entry->valid = false;
  return entry;
}

int blk_read_cached(struct block_device *dev, u64 lba, u32 count,
                    void *buffer) {
  if (!dev || !dev->read_blocks)
    return -1;
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

int blk_write_cached(struct block_device *dev, u64 lba, u32 count,
                     const void *buffer) {
  if (!dev || !dev->write_blocks)
    return -1;
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

    /* Write-Back: Mark as dirty, DO NOT write to disk immediately */
    entry->dirty = true;
  }
  return 0;
}

/* POSIX: Fsync/Sync support - Flush all dirty blocks to physical storage */
void blk_cache_flush(struct block_device *dev) {
  if (!dev) {
    /* Flush all devices if dev is NULL */
    for (usize i = 0; i < blk_device_count; i++) {
      blk_cache_flush(blk_devices[i]);
    }
    return;
  }
  if (!dev->write_blocks)
    return;

  for (int i = 0; i < CACHE_ENTRIES; i++) {
    if (bcache[i].valid && bcache[i].dev == dev && bcache[i].dirty) {
      dev->write_blocks(dev, bcache[i].lba, 1, bcache[i].data);
      bcache[i].dirty = false;
    }
  }
}
