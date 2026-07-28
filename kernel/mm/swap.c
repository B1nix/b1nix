#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <string.h>

/*
 * Simple swap system.
 * Uses a block device (e.g. a disk partition or a swap file) to store evicted pages.
 * Each page is stored at a fixed LBA offset: swap_lba = SWAP_START + slot_index * 8
 * (since PAGE_SIZE / 512 = 8 sectors per page).
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
 * swap_in. Idempotent on an already-free slot. */
void swap_free_slot_index(u32 slot_index)
{
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
 * The slot index is the ONLY identity the caller needs — it stores it directly
 * in the page's (non-present) PTE, so no (pml4,vaddr) reverse map is kept. The
 * page goes through the block cache; we do NOT force a synchronous flush — the
 * bcache keeps the dirty block, so a swap_in before it is written back simply
 * reads the cached copy, and the Variant-D throttle/eviction persists it. */
int swap_out(u64 physical_frame)
{
    if (!swap_active())
        return -1;

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
    extern u64 vmm_direct_map_base(void);
    u64 direct_base = vmm_direct_map_base();
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

