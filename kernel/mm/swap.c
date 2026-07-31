#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <string.h>

/*
 * Simple swap system.
 * Uses a block device (e.g. a disk partition or a swap file) to store evicted pages.
 * Each page is stored at a fixed LBA offset: swap_lba = SWAP_START + slot_index * 8
 * (since PAGE_SIZE / 512 = 8 sectors per page).
 *
 * ZSWAP-lite: before a page is written to disk, it is compressed with LZ4 into
 * a bounded RAM pool. Pages that compress to <= half a page stay entirely in
 * RAM — no disk I/O on the eviction path — and are decompressed on swap_in.
 * When the pool is full (or a page is incompressible), the page falls back to
 * the disk slot path. Pool entries are referenced from the swapped PTE through
 * the same slot field, marked with ZSWAP_SLOT_COMPRESSED. There is no reverse
 * map, so pool entries are only reclaimed on access/teardown (no LRU demotion).
 */

/* Swap slot table sizing (B3 audit): allocate on demand at
 * vmm_set_swap_device() with the exact slot count the backing device can
 * hold, clamped to a sane ceiling so a malicious or huge swap volume can't
 * cost arbitrary kernel memory. ~24 bytes per entry. The previous 65536-
 * slot static table cost ~1.5 MiB BSS on every machine regardless of
 * whether swap was wired up. */
#define SECTORS_PER_PAGE (PAGE_SIZE / 512)
/* Swap metadata is a 1-bit-per-slot allocation BITMAP — not a fat per-slot
 * table. The old table stored (pml4_phys, virtual_addr) per slot (24 B) so
 * swap_in had to LINEAR-SCAN it to find a page's slot, and the table's RAM cost
 * capped swap at ~5x RAM. Instead, the slot index is stored directly in the
 * swapped page's (non-present) PTE address bits, so swap_in reads it in O(1) and
 * needs no reverse map — leaving just a used/free bitmap. At 1 bit/slot the
 * bitmap is sized to the whole device, bounded only by a small RAM fraction:
 * a 512 MiB box can index ~512 GiB of swap with a 16 MiB bitmap (vs ~2.7 GiB
 * before). No fixed ceiling — exactly the "no hardcoded caps" goal. */
#define SWAP_BITMAP_RAM_DIVISOR 32 /* bitmap uses <= ~3% of usable RAM */
#define SWAP_SLOTS_MIN 4096        /* always allow at least ~16 MiB of swap */

static struct block_device *swap_dev = 0;
static u64 swap_start_lba = 0; // First LBA of swap area
static u64 swap_sector_count = 0;

static u8 *swap_bitmap = 0;        /* 1 bit per slot: 1 = used */
static usize swap_slot_count = 0;  /* usable slots, set by vmm_set_swap_device */
static usize swap_used = 0;        /* allocated slots (for diagnostics) */
static usize swap_next_slot = 0;   /* round-robin allocation cursor */

static int swap_bit_get(usize i) {
    return (swap_bitmap[i >> 3] >> (i & 7)) & 1;
}
static void swap_bit_set(usize i) { swap_bitmap[i >> 3] |= (u8)(1u << (i & 7)); }
static void swap_bit_clear(usize i) { swap_bitmap[i >> 3] &= (u8)~(1u << (i & 7)); }

/* ─────────────────────────── ZSWAP-lite ────────────────────────────────
 * A bounded RAM pool of LZ4-compressed swapped-out pages. A PTE slot value
 * carrying ZSWAP_SLOT_COMPRESSED addresses a pool entry instead of a disk
 * slot. Disk slots are clamped below 1 << 30 (the flag bit), which a single
 * swap device cannot reach in practice (a 1 PiB device would be needed).
 * ─────────────────────────────────────────────────────────────────────── */
#define ZSWAP_POOL_RAM_DIVISOR 32   /* pool <= ~3% of usable RAM */
#define ZSWAP_SLOT_COMPRESSED (1u << 30)
#define ZSWAP_MAX_ENTRIES 65536
#define ZSWAP_MIN_ENTRIES 64
#define ZSWAP_HASH_LOG 15           /* 32 K entries -> 64 KiB hash table */
#define LZ4_COMPRESS_BOUND(n) ((n) + ((n) / 255) + 16)

struct zswap_entry {
    u8 *data;   /* kmalloc'd compressed blob */
    u16 size;
};

static struct zswap_entry *zswap_pool = 0;
static u8 *zswap_pool_used = 0;     /* 1 bit per entry */
static usize zswap_pool_count = 0;
static usize zswap_pool_used_n = 0;
static spinlock_t zswap_pool_lock = 0;
static u16 *zswap_hash = 0;         /* compressor scratch (serialized by the lock) */

static int zswap_pool_used_bit(usize i) {
    return (zswap_pool_used[i >> 3] >> (i & 7)) & 1;
}
static void zswap_pool_used_set(usize i) {
    zswap_pool_used[i >> 3] |= (u8)(1u << (i & 7));
}
static void zswap_pool_used_clear(usize i) {
    zswap_pool_used[i >> 3] &= (u8)~(1u << (i & 7));
}

/* LZ4 block-format hash of the next 4 bytes. Unaligned-safe. */
static u32 lz4_hash4(const u8 *p) {
    u32 v;
    memcpy(&v, p, 4);
    v *= 0x9E3779B1u;
    return (v >> (32 - ZSWAP_HASH_LOG)) & ((1u << ZSWAP_HASH_LOG) - 1);
}

/* LZ4 block-format compressor (greedy, single pass). Returns the compressed
 * size (>0), or 0 when the input cannot be represented within dstCapacity
 * (treated as incompressible by the caller). Pure integer code — no SSE, no
 * floating point — safe for the kernel's -mno-sse build. Uses the shared
 * zswap_hash scratch table; the caller must serialize access (zswap_pool_lock). */
static int lz4_compress_block(const u8 *src, u8 *dst, int srcSize, int dstCapacity) {
    const int HT = 1 << ZSWAP_HASH_LOG;
    u16 *ht = zswap_hash;
    for (int i = 0; i < HT; i++) ht[i] = 0xFFFF;

    int ip = 0;      /* current position in src */
    int anchor = 0;  /* start of pending literals */
    int op = 0;      /* write position in dst */

    while (ip < srcSize - 4) {
        u32 h = lz4_hash4(src + ip);
        u16 cand = ht[h];
        ht[h] = (u16)ip;
        if (cand != 0xFFFF && ip - cand > 0 && ip - cand < 0x10000) {
            u32 a, b;
            memcpy(&a, src + cand, 4);
            memcpy(&b, src + ip, 4);
            if (a == b) {
                int len = 4;
                while (ip + len < srcSize && src[cand + len] == src[ip + len])
                    len++;
                int litLen = ip - anchor;
                int mlen = len - 4;
                if (op + 1 + litLen / 255 + litLen + 2 + mlen / 255 > dstCapacity)
                    return 0;
                int tok = op++;
                dst[tok] = (u8)((litLen >= 15 ? 15 : litLen) << 4);
                if (litLen >= 15) {
                    int e = litLen - 15;
                    while (e >= 255) { dst[op++] = 255; e -= 255; }
                    dst[op++] = (u8)e;
                }
                memcpy(dst + op, src + anchor, (usize)litLen);
                op += litLen;
                dst[tok] |= (u8)(mlen >= 15 ? 15 : mlen);
                int off = ip - cand;
                dst[op++] = (u8)off;
                dst[op++] = (u8)(off >> 8);
                if (mlen >= 15) {
                    int e = mlen - 15;
                    while (e >= 255) { dst[op++] = 255; e -= 255; }
                    dst[op++] = (u8)e;
                }
                ip += len;
                anchor = ip;
                continue;
            }
        }
        ip++;
    }

    /* Trailing literals — the last LZ4 sequence carries no match. */
    int litLen = srcSize - anchor;
    if (op + 1 + litLen / 255 + litLen > dstCapacity) return 0;
    dst[op++] = (u8)((litLen >= 15 ? 15 : litLen) << 4);
    if (litLen >= 15) {
        int e = litLen - 15;
        while (e >= 255) { dst[op++] = 255; e -= 255; }
        dst[op++] = (u8)e;
    }
    memcpy(dst + op, src + anchor, (usize)litLen);
    op += litLen;
    return op;
}

/* LZ4 block-format decompressor. Decodes exactly dstSize bytes (raw-block
 * convention: the final sequence is literals only). Returns 0 on success, -1
 * on corrupt input (bounds-checked against srcSize). */
static int lz4_decompress_block(const u8 *src, int srcSize, u8 *dst, int dstSize) {
    int ip = 0;
    int op = 0;
    for (;;) {
        if (ip >= srcSize) return -1;
        u8 token = src[ip++];
        int litLen = token >> 4;
        if (litLen == 15) {
            for (;;) {
                if (ip >= srcSize) return -1;
                u8 b = src[ip++];
                litLen += b;
                if (b != 255) break;
            }
        }
        if (op + litLen > dstSize || ip + litLen > srcSize) return -1;
        memcpy(dst + op, src + ip, (usize)litLen);
        ip += litLen;
        op += litLen;
        if (op == dstSize) return 0;   /* block end: trailing literals */
        if (ip + 2 > srcSize) return -1;
        int off = src[ip] | (src[ip + 1] << 8);
        ip += 2;
        if (off == 0 || off > op) return -1;
        int mlen = (token & 0xF) + 4;
        if ((token & 0xF) == 15) {
            for (;;) {
                if (ip >= srcSize) return -1;
                u8 b = src[ip++];
                mlen += b;
                if (b != 255) break;
            }
        }
        if (op + mlen > dstSize) return -1;
        for (int i = 0; i < mlen; i++)   /* overlapping matches allowed */
            dst[op + i] = dst[op + i - off];
        op += mlen;
    }
}

static void zswap_init(void) {
    usize budget = pmm_total_usable_memory() / ZSWAP_POOL_RAM_DIVISOR;
    if (budget < PAGE_SIZE) return;
    zswap_pool_count = budget / (PAGE_SIZE / 2);
    if (zswap_pool_count > ZSWAP_MAX_ENTRIES) zswap_pool_count = ZSWAP_MAX_ENTRIES;
    if (zswap_pool_count < ZSWAP_MIN_ENTRIES) zswap_pool_count = ZSWAP_MIN_ENTRIES;
    zswap_pool = kzalloc(zswap_pool_count * sizeof(struct zswap_entry));
    zswap_pool_used = kzalloc((zswap_pool_count + 7) / 8);
    zswap_hash = kmalloc((1u << ZSWAP_HASH_LOG) * sizeof(u16));
    if (!zswap_pool || !zswap_pool_used || !zswap_hash) {
        kfree(zswap_pool);
        kfree(zswap_pool_used);
        kfree(zswap_hash);
        zswap_pool = 0;
        zswap_pool_used = 0;
        zswap_hash = 0;
        zswap_pool_count = 0;
        return;
    }
    console_write("zswap: compressed pool ");
    console_write_dec(zswap_pool_count);
    console_write(" entries (");
    console_write_dec(budget / 1024);
    console_write(" KiB budget)\n");
}

/* Compress `page` into a pool entry. Returns the encoded slot (with
 * ZSWAP_SLOT_COMPRESSED) on success, or -1 to fall back to the disk path. */
static int zswap_pool_store(const u8 *page) {
    u64 flags;
    spin_lock_irqsave(&zswap_pool_lock, &flags);
    usize idx = zswap_pool_count;
    for (usize i = 0; i < zswap_pool_count; i++) {
        if (!zswap_pool_used_bit(i)) { idx = i; break; }
    }
    if (idx == zswap_pool_count) {
        spin_unlock_irqrestore(&zswap_pool_lock, flags);
        return -1;   /* pool full */
    }
    u8 *blob = kmalloc(LZ4_COMPRESS_BOUND(PAGE_SIZE));
    if (!blob) {
        spin_unlock_irqrestore(&zswap_pool_lock, flags);
        return -1;
    }
    int csize = lz4_compress_block(page, blob, PAGE_SIZE, LZ4_COMPRESS_BOUND(PAGE_SIZE));
    if (csize <= 0 || (usize)csize > PAGE_SIZE / 2) {
        /* Incompressible or worse than 2:1 — disk is the better home. */
        kfree(blob);
        spin_unlock_irqrestore(&zswap_pool_lock, flags);
        return -1;
    }
    zswap_pool[idx].data = blob;
    zswap_pool[idx].size = (u16)csize;
    zswap_pool_used_set(idx);
    zswap_pool_used_n++;
    spin_unlock_irqrestore(&zswap_pool_lock, flags);
    return (int)(idx | ZSWAP_SLOT_COMPRESSED);
}

/* Decompress pool entry `slot` into `page` and free the entry. */
static int zswap_pool_load(u32 slot, u8 *page) {
    u32 idx = slot & ~ZSWAP_SLOT_COMPRESSED;
    if (idx >= zswap_pool_count) return -1;
    u64 flags;
    spin_lock_irqsave(&zswap_pool_lock, &flags);
    if (!zswap_pool_used_bit(idx)) {
        spin_unlock_irqrestore(&zswap_pool_lock, flags);
        return -1;
    }
    struct zswap_entry *e = &zswap_pool[idx];
    if (lz4_decompress_block(e->data, e->size, page, PAGE_SIZE) != 0) {
        spin_unlock_irqrestore(&zswap_pool_lock, flags);
        return -1;
    }
    kfree(e->data);
    e->data = 0;
    e->size = 0;
    zswap_pool_used_clear(idx);
    zswap_pool_used_n--;
    spin_unlock_irqrestore(&zswap_pool_lock, flags);
    return 0;
}

/* Release a pool entry by its encoded slot (no disk bitmap involved). */
static void zswap_pool_free(u32 slot) {
    u32 idx = slot & ~ZSWAP_SLOT_COMPRESSED;
    if (idx >= zswap_pool_count) return;
    u64 flags;
    spin_lock_irqsave(&zswap_pool_lock, &flags);
    if (zswap_pool_used_bit(idx)) {
        kfree(zswap_pool[idx].data);
        zswap_pool[idx].data = 0;
        zswap_pool[idx].size = 0;
        zswap_pool_used_clear(idx);
        zswap_pool_used_n--;
    }
    spin_unlock_irqrestore(&zswap_pool_lock, flags);
}

void vmm_set_swap_device(struct block_device *dev)
{
    swap_dev = dev;
    if (dev) {
        if (strcmp(dev->name, "sata1") == 0 || strcmp(dev->name, "nvme1") == 0) {
            swap_start_lba = 0;
            swap_sector_count = dev->block_count;
        } else {
            // Reserve last 1/4 of the device for swap
            swap_start_lba = (dev->block_count * 3) / 4;
            swap_sector_count = dev->block_count - swap_start_lba;
        }
        /* Size the bitmap to the WHOLE device, bounded only by the RAM the
         * bitmap itself may cost (a fraction of usable RAM). At 1 bit/slot this
         * is tiny — a 2 GiB swap disk needs a 64 KiB bitmap; even a 1 TiB disk
         * on a 512 MiB box is capped at a 16 MiB bitmap (~512 GiB of usable
         * swap). No fixed slot ceiling. */
        usize dev_slots = (usize)(swap_sector_count / SECTORS_PER_PAGE);
        usize ram_bitmap_bytes = (usize)(pmm_total_usable_memory() / SWAP_BITMAP_RAM_DIVISOR);
        usize ram_slots = ram_bitmap_bytes * 8; /* 8 slots per bitmap byte */
        if (ram_slots < SWAP_SLOTS_MIN)
            ram_slots = SWAP_SLOTS_MIN;
        swap_slot_count = dev_slots < ram_slots ? dev_slots : ram_slots;
        /* Keep disk slot indices clear of the ZSWAP compressed flag bit. A
         * real swap device cannot reach 1 << 30 slots (4 TiB of swap), but
         * clamp anyway so the PTE encoding is unambiguous. */
        if (swap_slot_count > (usize)ZSWAP_SLOT_COMPRESSED - 1)
            swap_slot_count = (usize)ZSWAP_SLOT_COMPRESSED - 1;
        swap_used = 0;
        swap_next_slot = 0;
        swap_bitmap = kzalloc((swap_slot_count + 7) / 8);
        if (!swap_bitmap) {
            console_write("swap: bitmap alloc failed, disabling swap\n");
            swap_slot_count = 0;
        }
        console_write("swap: device=");
        console_write(dev->name);
        console_write(" start_lba=");
        console_write_dec(swap_start_lba);
        console_write(" sectors=");
        console_write_dec(swap_sector_count);
        console_write(" slots=");
        console_write_dec(swap_slot_count);
        console_write(" bitmap=");
        console_write_dec((swap_slot_count + 7) / 8 / 1024);
        console_write("KiB\n");
    }
}

int swap_init(void)
{
    /* Slot table is allocated on demand once a swap device is attached
     * (see vmm_set_swap_device). No device means no swap memory cost. */
    console_write("swap: initialized (slot table allocated when device attached)\n");

    zswap_init();

    struct block_device *dev = blk_get("sata1");
    if (!dev) {
        dev = blk_get("nvme1");
    }
    if (dev) {
        vmm_set_swap_device(dev);
    }

    if (swap_dev) {
        console_write("swap: running internal smoke test...\n");
        u64 test_frame = pmm_alloc_frame();
        if (test_frame) {
            extern u64 vmm_direct_map_base(void);
            u64 direct_base = vmm_direct_map_base();
            char *ptr = (char *)(usize)(test_frame + direct_base);
            strcpy(ptr, "B1NIX Swap Smoke Test Pattern");

            int slot = swap_out(test_frame);
            if (slot >= 0) {
                console_write("swap: page swap-out ok, slot=");
                console_write_dec(slot);
                console_write("\n");

                memset(ptr, 0, PAGE_SIZE);

                u64 out_frame = 0;
                if (swap_in((u32)slot, &out_frame) == 0 && out_frame != 0) {
                    char *out_ptr = (char *)(usize)(out_frame + direct_base);
                    if (strcmp(out_ptr, "B1NIX Swap Smoke Test Pattern") == 0) {
                        console_write("swap: page swap-in ok, verified data\n");
                        console_write("M14-SMOKE: ok swap-smoke\n");
                    } else {
                        console_write("swap: swap-in data mismatch!\n");
                    }
                    pmm_free_frame(out_frame);
                } else {
                    console_write("swap: swap-in failed!\n");
                }
            } else {
                console_write("swap: swap-out failed!\n");
            }
            pmm_free_frame(test_frame);
        } else {
            console_write("swap: failed to allocate test frame\n");
        }
    } else {
        console_write("M14-SMOKE: swap not active (no device)\n");
    }

    return 0;
}

static u32 swap_alloc_slot(void)
{
    for (usize i = 0; i < swap_slot_count; i++) {
        usize idx = (swap_next_slot + i) % swap_slot_count;
        if (!swap_bit_get(idx)) {
            swap_bit_set(idx);
            swap_used++;
            swap_next_slot = (idx + 1) % swap_slot_count;
            return (u32)idx;
        }
    }
    return (u32)-1; // No free slots
}

/* Public: free a slot by index. Called by the address-space teardown walk
 * (paging_free_swap_slots) for every VMM_SWAPPED PTE, and internally by
 * swap_in. Handles both disk slots and ZSWAP pool entries. Idempotent on an
 * already-free slot. */
void swap_free_slot_index(u32 slot_index)
{
    if (slot_index & ZSWAP_SLOT_COMPRESSED) {
        zswap_pool_free(slot_index);
        return;
    }
    if (slot_index < swap_slot_count && swap_bit_get(slot_index)) {
        swap_bit_clear(slot_index);
        if (swap_used)
            swap_used--;
    }
}

int swap_active(void)
{
    return swap_dev && swap_dev->write_blocks;
}

/* swapoff(2): detach the swap device. The caller must have paged every
 * swapped-out page back in first (paging_swap_in_all_swapped per address
 * space) — if any slot is still allocated the device is still in use and we
 * refuse, exactly as Linux does when swapoff cannot free the area. Frees the
 * allocation bitmap so a later swapon re-sizes it for its own device. */
int swap_detach(void)
{
    if (!swap_dev)
        return -1;
    if (swap_used != 0 || zswap_pool_used_n != 0)
        return -2; /* pages still live in swap -> caller reports EBUSY */
    if (swap_bitmap) {
        kfree(swap_bitmap);
        swap_bitmap = 0;
    }
    swap_dev = 0;
    swap_start_lba = 0;
    swap_sector_count = 0;
    swap_slot_count = 0;
    swap_next_slot = 0;
    console_write("swap: device detached\n");
    return 0;
}

/* Slot accounting for /proc/swaps and sysinfo(2). One slot is one page. */
int swap_stats(u64 *out_total_slots, u64 *out_used_slots)
{
    if (!swap_active())
        return -1;
    if (out_total_slots) *out_total_slots = (u64)swap_slot_count;
    if (out_used_slots) *out_used_slots = (u64)swap_used;
    return 0;
}

/* Write a frame to a freshly allocated swap slot and return its index, or -1.
 * ZSWAP-lite first tries to keep the page in RAM (compressed): if it succeeds
 * the returned slot carries ZSWAP_SLOT_COMPRESSED and no disk I/O happens at
 * all. Otherwise a disk slot is allocated as before. The slot index is the
 * ONLY identity the caller needs — it stores it directly in the page's
 * (non-present) PTE, so no (pml4,vaddr) reverse map is kept. Disk pages go
 * through the block cache; we do NOT force a synchronous flush — the bcache
 * keeps the dirty block, so a swap_in before it is written back simply reads
 * the cached copy, and the Variant-D throttle/eviction persists it. */
int swap_out(u64 physical_frame)
{
    if (!swap_active())
        return -1;

    extern u64 vmm_direct_map_base(void);
    u64 direct_base = vmm_direct_map_base();
    const u8 *page = (const u8 *)(usize)(physical_frame + direct_base);

    /* ZSWAP-lite: compressible pages never touch the disk. */
    if (zswap_pool) {
        int zslot = zswap_pool_store(page);
        if (zslot >= 0)
            return zslot;
    }

    u32 slot = swap_alloc_slot();
    if (slot == (u32)-1) {
        console_write("swap_out: no free swap slots\n");
        return -1;
    }

    u64 slot_sector = (u64)slot * SECTORS_PER_PAGE;
    if (slot_sector > swap_sector_count || SECTORS_PER_PAGE > swap_sector_count - slot_sector) {
        console_write("swap_out: slot exceeds swap device bounds\n");
        swap_free_slot_index(slot);
        return -1;
    }

    u64 lba = swap_start_lba + slot_sector;
    int ret = blk_write_cached(swap_dev, lba, SECTORS_PER_PAGE,
                               (const void *)(usize)(physical_frame + direct_base));
    if (ret < 0) {
        swap_free_slot_index(slot);
        return -1;
    }
    return (int)slot;
}

/* Read the page stored at `slot` into a fresh frame, free the slot, return 0.
 * The caller extracts `slot` from the faulting VMM_SWAPPED PTE. */
int swap_in(u32 slot, u64 *out_physical_frame)
{
    if (!swap_dev || !swap_dev->read_blocks)
        return -1;

    /* ZSWAP-lite pool entry: decompress straight into the new frame. */
    if (slot & ZSWAP_SLOT_COMPRESSED) {
        u32 idx = slot & ~ZSWAP_SLOT_COMPRESSED;
        if (!zswap_pool || idx >= zswap_pool_count || !zswap_pool_used_bit(idx))
            return -1; /* not a live pool entry */
        u64 frame = pmm_alloc_frame();
        if (!frame)
            return -1;
        extern u64 vmm_direct_map_base(void);
        u64 direct_base = vmm_direct_map_base();
        if (zswap_pool_load(slot, (u8 *)(usize)(frame + direct_base)) == 0) {
            *out_physical_frame = frame;
            return 0;
        }
        pmm_free_frame(frame);
        return -1;
    }

    if (slot >= swap_slot_count || !swap_bit_get(slot))
        return -1; /* not a live swap slot */

    u64 lba = swap_start_lba + (u64)slot * SECTORS_PER_PAGE;
    u64 frame = pmm_alloc_frame();
    if (!frame)
        return -1;

    extern u64 vmm_direct_map_base(void);
    u64 direct_base = vmm_direct_map_base();
    int ret = blk_read_cached(swap_dev, lba, SECTORS_PER_PAGE,
                              (void *)(usize)(frame + direct_base));
    if (ret < 0) {
        pmm_free_frame(frame);
        return -1;
    }

    *out_physical_frame = frame;
    swap_free_slot_index(slot);
    return 0;
}

/* Free every swap slot owned by an exiting address space. The slot ownership
 * lives in the PTEs (slot index encoded in each VMM_SWAPPED leaf), so walk the
 * page tables rather than a reverse-map table. paging_free_swap_slots calls
 * swap_free_slot_index for each VMM_SWAPPED PTE it finds. */
void swap_free_all_slots(u64 pml4_phys)
{
    extern void paging_free_swap_slots(u64 pml4_phys);
    paging_free_swap_slots(pml4_phys);
}

