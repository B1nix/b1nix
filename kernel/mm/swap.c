/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/blk.h>
#include <b1nix/cgroup.h>
#include <b1nix/console.h>
#include <b1nix/lz4.h>
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

static int swap_device_is_free(struct block_device *dev);

static struct block_device *swap_dev = 0;
static u64 swap_start_lba = 0; // First LBA of swap area
static u64 swap_sector_count = 0;

static u8 *swap_bitmap = 0;        /* 1 bit per slot: 1 = used */
static usize swap_slot_count = 0;  /* usable slots, set by vmm_set_swap_device */
static usize swap_used = 0;        /* allocated slots (for diagnostics) */
static usize swap_next_slot = 0;   /* round-robin allocation cursor */
static int swap_full_said = 0;     /* the "device is full" line, said once */

/* Who each slot is charged to (kernel/fs/cgroup/cgroup.c hands out the ids;
 * 0 means "nobody", which is the root cgroup and every machine that never
 * mounts cgroup2).
 *
 * A swapped page has to stay charged to the cgroup that owned it, because
 * memory.swap.current is read long after the task that faulted it has gone to
 * sleep, and the page comes back in whatever context happens to touch it. The
 * PTE carries the slot index and nothing else, so the attribution lives here,
 * beside the slot -- two bytes per slot, the same place and the same price
 * Linux pays for its swap_cgroup array. */
static u16 *swap_owner = 0;        /* one id per disk slot */
static u16 *zswap_owner = 0;       /* one id per pool entry */

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

/* Blobs whose entry is gone but whose memory has not been handed back yet.
 *
 * Freeing one is a kfree, and a slot is freed from places that hold locks the
 * heap lock must not be taken under. The address-space teardown is the one
 * that matters: it walks the page tables with the paging lock held and frees
 * every swapped page's slot as it goes, so a kfree there is
 *     paging lock -> heap lock
 * while a kernel heap that grows takes
 *     heap lock -> paging lock (to map the new pages)
 * and the two together are a deadlock that takes down the machine the first
 * time a process with a lot of compressed pages exits.
 *
 * So a freed blob goes on this list instead, threaded through its own first
 * eight bytes, and the list is drained from places that hold nothing. Nothing
 * here allocates, so pushing is always safe. */
static u8 *zswap_free_pending;
static spinlock_t zswap_pending_lock = 0;

/* Every stored blob is at least this big, so the list can live inside one. */
#define ZSWAP_MIN_BLOB (sizeof(void *))

static void zswap_pending_push(u8 *blob) {
    if (!blob)
        return;

    u64 flags;

    spin_lock_irqsave(&zswap_pending_lock, &flags);
    *(u8 **)(void *)blob = zswap_free_pending;
    zswap_free_pending = blob;
    spin_unlock_irqrestore(&zswap_pending_lock, flags);
}

/* Hand back everything that was freed while a lock was held. Called from the
 * paths that hold nothing: storing a page, and reading one back. */
static void zswap_drain_pending(void) {
    u64 flags;
    u8 *chain;

    spin_lock_irqsave(&zswap_pending_lock, &flags);
    chain = zswap_free_pending;
    zswap_free_pending = 0;
    spin_unlock_irqrestore(&zswap_pending_lock, flags);

    while (chain) {
        u8 *next = *(u8 **)(void *)chain;

        kfree(chain);
        chain = next;
    }
}

static int zswap_pool_used_bit(usize i) {
    return (zswap_pool_used[i >> 3] >> (i & 7)) & 1;
}
static void zswap_pool_used_set(usize i) {
    zswap_pool_used[i >> 3] |= (u8)(1u << (i & 7));
}
static void zswap_pool_used_clear(usize i) {
    zswap_pool_used[i >> 3] &= (u8)~(1u << (i & 7));
}

static void zswap_init(void) {
    usize budget = pmm_total_usable_memory() / ZSWAP_POOL_RAM_DIVISOR;
    if (budget < PAGE_SIZE) return;
    zswap_pool_count = budget / (PAGE_SIZE / 2);
    if (zswap_pool_count > ZSWAP_MAX_ENTRIES) zswap_pool_count = ZSWAP_MAX_ENTRIES;
    if (zswap_pool_count < ZSWAP_MIN_ENTRIES) zswap_pool_count = ZSWAP_MIN_ENTRIES;
    zswap_pool = kzalloc(zswap_pool_count * sizeof(struct zswap_entry));
    zswap_pool_used = kzalloc((zswap_pool_count + 7) / 8);
    zswap_hash = kmalloc(LZ4_HASH_ENTRIES * sizeof(u16));
    zswap_owner = kzalloc(zswap_pool_count * sizeof(u16));
    if (!zswap_pool || !zswap_pool_used || !zswap_hash || !zswap_owner) {
        kfree(zswap_pool);
        kfree(zswap_pool_used);
        kfree(zswap_hash);
        kfree(zswap_owner);
        zswap_pool = 0;
        zswap_pool_used = 0;
        zswap_hash = 0;
        zswap_owner = 0;
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
    /* Allocated BEFORE the lock, and the failed cases free it after letting
     * go. A kmalloc under this lock is a kmalloc with interrupts off inside
     * the reclaim path: the heap may have to grow, growing it maps frames, and
     * everything else on the machine waits on the heap lock meanwhile -- which
     * is reported, eventually, as a spinlock lockup somewhere unrelated. */
    zswap_drain_pending();

    u8 *blob = kmalloc(LZ4_COMPRESS_BOUND(PAGE_SIZE));

    if (!blob)
        return -1;

    u64 flags;
    spin_lock_irqsave(&zswap_pool_lock, &flags);
    usize idx = zswap_pool_count;
    for (usize i = 0; i < zswap_pool_count; i++) {
        if (!zswap_pool_used_bit(i)) { idx = i; break; }
    }
    if (idx == zswap_pool_count) {
        spin_unlock_irqrestore(&zswap_pool_lock, flags);
        kfree(blob);
        return -1;   /* pool full */
    }
    int csize = lz4_compress(page, PAGE_SIZE, blob, LZ4_COMPRESS_BOUND(PAGE_SIZE),
                             zswap_hash);
    if (csize > 0 && (usize)csize < ZSWAP_MIN_BLOB)
        csize = (int)ZSWAP_MIN_BLOB;  /* room for the pending-free link */
    if (csize <= 0 || (usize)csize > PAGE_SIZE / 2) {
        /* Incompressible or worse than 2:1 — disk is the better home. */
        spin_unlock_irqrestore(&zswap_pool_lock, flags);
        kfree(blob);
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
    if (lz4_decompress(e->data, e->size, page, PAGE_SIZE) != 0) {
        spin_unlock_irqrestore(&zswap_pool_lock, flags);
        return -1;
    }
    u8 *old = e->data;

    e->data = 0;
    e->size = 0;
    zswap_pool_used_clear(idx);
    zswap_pool_used_n--;
    spin_unlock_irqrestore(&zswap_pool_lock, flags);
    zswap_pending_push(old);
    zswap_drain_pending();  /* a swap-in holds nothing: a good place to pay */
    return 0;
}

/* Release a pool entry by its encoded slot (no disk bitmap involved). */
static void zswap_pool_free(u32 slot) {
    u32 idx = slot & ~ZSWAP_SLOT_COMPRESSED;
    if (idx >= zswap_pool_count) return;
    u64 flags;
    u8 *old = 0;

    spin_lock_irqsave(&zswap_pool_lock, &flags);
    if (zswap_pool_used_bit(idx)) {
        old = zswap_pool[idx].data;
        zswap_pool[idx].data = 0;
        zswap_pool[idx].size = 0;
        zswap_pool_used_clear(idx);
        zswap_pool_used_n--;
    }
    spin_unlock_irqrestore(&zswap_pool_lock, flags);
    /* NOT kfree: this runs from the address-space teardown, which holds the
     * paging lock. See zswap_free_pending. */
    zswap_pending_push(old);
}

/* The second disk of a storage class is the swap disk by convention: the first
 * carries a filesystem, the second is handed over whole. Asked for by bus and
 * position rather than by name — "sdb" stopped meaning "the second ATA disk"
 * once USB mass storage joined the same sd* sequence, and whether a USB stick
 * is plugged in must not decide which disk gets overwritten with swap. */
#define SWAP_DISK_INDEX 1

static struct block_device *swap_find_dedicated_disk(void)
{
    struct block_device *dev = blk_nth_on_bus(BLK_BUS_ATA, SWAP_DISK_INDEX);
    if (!dev)
        dev = blk_nth_on_bus(BLK_BUS_NVME, SWAP_DISK_INDEX);
    /* virtio, but only on a machine that has no ATA and no NVMe disk AT ALL.
     *
     * QEMU virt is such a machine: every disk arrives over virtio-mmio, so
     * there was no way to give that board a swap disk and every path behind
     * swap_active() went untested on it. The condition is "no such disk
     * anywhere" rather than "no second one" on purpose — where those buses do
     * exist, the second virtio disk is a scratch disk something else owns, and
     * handing it to swap would overwrite a filesystem under its user. The
     * first virtio disk is skipped for the same reason: it is the root. */
    if (!dev && !blk_nth_on_bus(BLK_BUS_ATA, 0) && !blk_nth_on_bus(BLK_BUS_NVME, 0))
        dev = blk_nth_on_bus(BLK_BUS_VIRTIO, SWAP_DISK_INDEX);
    if (dev && !swap_device_is_free(dev))
        return 0;
    return dev;
}

/*
 * Does this disk already belong to something?
 *
 * The picker below takes the second virtio disk on a machine with no ATA and
 * no NVMe, and then reserves its last quarter -- so attaching a scratch disk
 * with a filesystem on it to such a guest hands a quarter of that filesystem
 * to swap. Seen with a btrfs image attached to the Arch VM: the kernel
 * announced "swap: device=vdb" over a filesystem systemd was about to mount,
 * and only the absence of any swapping that boot kept the image intact.
 *
 * A device is free if it carries a swap signature (it was made for this) or
 * carries no filesystem this kernel can recognise. Anything else keeps its
 * disk.
 */
static int swap_device_is_free(struct block_device *dev)
{
    u8 buf[512];
    int recognised = 0;

    if (!dev || dev->block_count < 8)
        return 0;

    /* "SWAPSPACE2" sits at the end of the first page of a mkswap'd device. */
    if (blk_read_cached(dev, 7, 1, buf) >= 0 &&
        memcmp(buf + 512 - 10, "SWAPSPACE2", 10) == 0)
        return 1;

    /* ext2/3/4: magic 0xEF53 at byte 1080 (LBA 2, offset 56). */
    if (blk_read_cached(dev, 2, 1, buf) >= 0 &&
        buf[56] == 0x53 && buf[57] == 0xef)
        recognised = 1;

    /* btrfs: "_BHRfS_M" at byte 65600 (LBA 128, offset 64). */
    if (!recognised && blk_read_cached(dev, 128, 1, buf) >= 0 &&
        memcmp(buf + 64, "_BHRfS_M", 8) == 0)
        recognised = 1;

    /* FAT and XFS both name themselves in the first sector. */
    if (!recognised && blk_read_cached(dev, 0, 1, buf) >= 0 &&
        (memcmp(buf + 54, "FAT", 3) == 0 || memcmp(buf + 82, "FAT32", 5) == 0 ||
         memcmp(buf, "XFSB", 4) == 0))
        recognised = 1;

    if (recognised) {
        console_write("swap: ");
        console_write(dev->name);
        console_write(" carries a filesystem — leaving it alone\n");
        return 0;
    }
    return 1;
}

/* Does this device carry a mkswap(8) signature? Then userspace has already
 * said what the whole of it is for, and reserving a corner of it would be
 * second-guessing a decision that has been written to the device. */
static int swap_has_signature(struct block_device *dev)
{
    u8 buf[512];

    if (!dev || dev->block_count < SECTORS_PER_PAGE)
        return 0;
    return blk_read_cached(dev, SECTORS_PER_PAGE - 1, 1, buf) >= 0 &&
           memcmp(buf + 512 - 10, "SWAPSPACE2", 10) == 0;
}

static int swap_is_dedicated_disk(struct block_device *dev)
{
    if (!dev)
        return 0;
    /* A device that says it is swap IS swap, whatever bus it is on. This is
     * the only way `mkswap /dev/zram0 && swapon /dev/zram0` can mean what it
     * says: zram is not on one of the two buses named below, and taking the
     * last quarter of it would hand back three quarters of a device that
     * exists for nothing else. */
    if (swap_has_signature(dev))
        return 1;
    return dev == blk_nth_on_bus(BLK_BUS_ATA, SWAP_DISK_INDEX) ||
           dev == blk_nth_on_bus(BLK_BUS_NVME, SWAP_DISK_INDEX);
}

void vmm_set_swap_device(struct block_device *dev)
{
    swap_dev = dev;
    if (dev) {
        if (swap_is_dedicated_disk(dev)) {
            /* Skip the first page on a device that carries a header: that page
             * IS the header, and slot 0 would write over the signature that
             * told us to use the whole device -- so the next swapon of the
             * same device would take a quarter of it instead. */
            swap_start_lba = swap_has_signature(dev) ? SECTORS_PER_PAGE : 0;
            swap_sector_count = dev->block_count - swap_start_lba;
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
        swap_owner = kzalloc(swap_slot_count * sizeof(u16));
        if (!swap_bitmap || !swap_owner) {
            console_write("swap: bitmap alloc failed, disabling swap\n");
            kfree(swap_bitmap);
            kfree(swap_owner);
            swap_bitmap = 0;
            swap_owner = 0;
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

    struct block_device *dev = swap_find_dedicated_disk();
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
    /* Skip whole full bytes at a stride, then let the CPU find the free bit.
     *
     * The scan below walked one bit at a time over every slot in the device —
     * on a full swap area that is the entire bitmap, per allocation, and swap
     * allocation happens exactly when the machine is already short of memory
     * and least able to spare the cycles. A byte with no free bit is 0xFF and
     * can be stepped over whole; within the first byte that has one,
     * __builtin_ctz names it in a single instruction (BSF/TZCNT).
     *
     * The wrap-around start point is kept, so allocation still walks forward
     * from the last slot handed out rather than always refilling the front. */
    for (usize i = 0; i < swap_slot_count; ) {
        usize idx = (swap_next_slot + i) % swap_slot_count;

        /* Byte-aligned and a whole byte left before the wrap: test all eight
         * at once and step over the byte if none of them is free. */
        if ((idx & 7) == 0 && i + 8 <= swap_slot_count &&
            idx + 8 <= swap_slot_count) {
            u8 byte = swap_bitmap[idx >> 3];
            if (byte == 0xFFu) {
                i += 8;
                continue;
            }
            idx += (usize)__builtin_ctz((unsigned)(u8)~byte);
        } else if (swap_bit_get(idx)) {
            i++;
            continue;
        }

        {
            swap_bit_set(idx);
            swap_used++;
            swap_next_slot = (idx + 1) % swap_slot_count;
            return (u32)idx;
        }
    }
    return (u32)-1; // No free slots
}

/* ── who a swapped page is charged to ───────────────────────────────────────
 * Set right after the page is written out, by the only caller that knows whose
 * page it was (kernel/mm/eviction.c walks the ring and holds the task), and
 * read back exactly once, when the slot is freed. Freeing is the single place
 * the charge can be released from, and every path that stops using a slot goes
 * through it: swap_in on a major fault, the address-space teardown walk, and
 * swapoff. */
void swap_set_owner(u32 slot_index, u16 cg_id)
{
    if (slot_index & ZSWAP_SLOT_COMPRESSED) {
        u32 idx = slot_index & ~ZSWAP_SLOT_COMPRESSED;
        if (zswap_owner && idx < zswap_pool_count)
            zswap_owner[idx] = cg_id;
        return;
    }
    if (swap_owner && slot_index < swap_slot_count)
        swap_owner[slot_index] = cg_id;
}

/* Read the owner and clear it in one step: a slot that is being freed must not
 * be able to release the same charge twice, however many times a caller asks
 * for it to be freed. */
static u16 swap_owner_take(u32 slot_index)
{
    u16 id = 0;

    if (slot_index & ZSWAP_SLOT_COMPRESSED) {
        u32 idx = slot_index & ~ZSWAP_SLOT_COMPRESSED;
        if (zswap_owner && idx < zswap_pool_count) {
            id = zswap_owner[idx];
            zswap_owner[idx] = 0;
        }
        return id;
    }
    if (swap_owner && slot_index < swap_slot_count) {
        id = swap_owner[slot_index];
        swap_owner[slot_index] = 0;
    }
    return id;
}

/* Public: free a slot by index. Called by the address-space teardown walk
 * (paging_free_swap_slots) for every VMM_SWAPPED PTE, and internally by
 * swap_in. Handles both disk slots and ZSWAP pool entries. Idempotent on an
 * already-free slot. */
void swap_free_slot_index(u32 slot_index)
{
    if (slot_index & ZSWAP_SLOT_COMPRESSED) {
        u32 idx = slot_index & ~ZSWAP_SLOT_COMPRESSED;
        /* Only a live entry carries a charge; freeing a free slot is a no-op
         * here as it is below. */
        int live = zswap_pool && idx < zswap_pool_count &&
                   zswap_pool_used_bit(idx);
        u16 owner = live ? swap_owner_take(slot_index) : 0;

        zswap_pool_free(slot_index);
        if (owner)
            cgroup_swap_uncharge(owner, 1);
        return;
    }
    if (slot_index < swap_slot_count && swap_bit_get(slot_index)) {
        u16 owner = swap_owner_take(slot_index);

        swap_bit_clear(slot_index);
        if (swap_used)
            swap_used--;
        if (owner)
            cgroup_swap_uncharge(owner, 1);
    }
}

int swap_active(void)
{
    return swap_dev && swap_dev->write_blocks;
}

/* The name of the device swap is using, for /proc/swaps. 0 when swap is off. */
const char *swap_device_name(void)
{
    return swap_active() && swap_dev ? swap_dev->name : 0;
}

/* Is this the device swap is currently using? Asked by a driver that is about
 * to take its backing store away (zram's reset), because the swap layer holds
 * the pointer and has pages out on it. */
int swap_is_device(struct block_device *dev)
{
    return dev && swap_dev == dev;
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
    if (swap_used != 0 || zswap_pool_used_n != 0) {
        /* Say what is holding it. A bare EBUSY from swapoff(8) is unactionable
         * -- the caller cannot see whether one page is left or a hundred
         * thousand, nor whether the number is going down. */
        console_write("swap: cannot detach, ");
        console_write_dec(swap_used);
        console_write(" disk slots and ");
        console_write_dec(zswap_pool_used_n);
        console_write(" compressed pages still in use\n");
        return -2; /* pages still live in swap -> caller reports EBUSY */
    }
    if (swap_bitmap) {
        kfree(swap_bitmap);
        swap_bitmap = 0;
    }
    if (swap_owner) {
        kfree(swap_owner);
        swap_owner = 0;
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
        /* Said once per full-to-not-full episode. A full swap device is the
         * state a machine under memory pressure stays in, and every eviction
         * attempt lands here -- thousands of identical lines that push the
         * evidence of what actually happened off the top of the log, and cost
         * more time on the serial port than the reclaim itself. */
        if (!swap_full_said) {
            swap_full_said = 1;
            console_write("swap: device is full\n");
        }
        return -1;
    }
    swap_full_said = 0;

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
            /* The entry is gone, so its charge must go with it. The disk path
             * below gets this from swap_free_slot_index; this path frees the
             * entry inside zswap_pool_load and would otherwise leave the
             * cgroup billed for a page that is back in memory -- and leave a
             * stale owner on a pool slot that the next eviction reuses. */
            u16 owner = swap_owner_take(slot);

            if (owner)
                cgroup_swap_uncharge(owner, 1);
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

