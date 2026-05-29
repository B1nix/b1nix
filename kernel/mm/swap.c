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
#define SWAP_SLOTS_MAX   65536
#define SECTORS_PER_PAGE (PAGE_SIZE / 512)

struct swap_slot_entry {
    u64 pml4_phys;         // Address space ID
    u64 virtual_addr;      // Page-aligned virtual address
    u32 slot_index;        // Index into swap area
    int used;
};

static struct block_device *swap_dev = 0;
static u64 swap_start_lba = 0; // First LBA of swap area
static u64 swap_sector_count = 0;

static struct swap_slot_entry *swap_table = 0;
static usize swap_slot_count = 0;  /* usable slots, set by vmm_set_swap_device */
static int swap_next_slot = 0;

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
        /* Allocate the slot table sized to the device, clamped to the
         * SWAP_SLOTS_MAX ceiling so a huge volume doesn't cost arbitrary
         * kernel memory. */
        usize dev_slots = (usize)(swap_sector_count / SECTORS_PER_PAGE);
        swap_slot_count = dev_slots < SWAP_SLOTS_MAX ? dev_slots : SWAP_SLOTS_MAX;
        swap_table = kzalloc(swap_slot_count * sizeof(struct swap_slot_entry));
        if (!swap_table) {
            console_write("swap: kzalloc slot table failed, disabling swap\n");
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
        console_write("\n");
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

            u64 fake_vaddr = 0xDEADBEEF000ULL;
            int slot = swap_out(0, fake_vaddr, test_frame);
            if (slot >= 0) {
                console_write("swap: page swap-out ok, slot=");
                console_write_dec(slot);
                console_write("\n");

                memset(ptr, 0, PAGE_SIZE);

                u64 out_frame = 0;
                if (swap_in(0, fake_vaddr, &out_frame) == 0 && out_frame != 0) {
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
        u32 idx = (u32)((swap_next_slot + i) % swap_slot_count);
        if (!swap_table[idx].used) {
            swap_table[idx].used = 1;
            swap_next_slot = (int)((idx + 1) % swap_slot_count);
            return idx;
        }
    }
    return (u32)-1; // No free slots
}

static void swap_free_slot(u32 slot_index)
{
    if (slot_index < swap_slot_count) {
        swap_table[slot_index].used = 0;
        swap_table[slot_index].virtual_addr = 0;
        swap_table[slot_index].pml4_phys = 0;
    }
}

static int swap_find_slot(u64 pml4_phys, u64 virtual_addr)
{
    for (usize i = 0; i < swap_slot_count; i++) {
        if (swap_table[i].used && swap_table[i].pml4_phys == pml4_phys &&
            swap_table[i].virtual_addr == virtual_addr) {
            return (int)i;
        }
    }
    return -1;
}

int swap_active(void)
{
    return swap_dev && swap_dev->write_blocks;
}

int swap_out(u64 pml4_phys, u64 virtual_addr, u64 physical_frame)
{
    if (!swap_active()) {
        return -1;
    }

    u32 slot = swap_alloc_slot();
    if (slot == (u32)-1) {
        console_write("swap_out: no free swap slots\n");
        return -1;
    }

    u64 slot_sector = (u64)slot * SECTORS_PER_PAGE;
    if (slot_sector > swap_sector_count || SECTORS_PER_PAGE > swap_sector_count - slot_sector) {
        console_write("swap_out: slot exceeds swap device bounds\n");
        swap_free_slot(slot);
        return -1;
    }

    u64 lba = swap_start_lba + slot_sector;

    // Write the page from physical memory to swap
    extern u64 vmm_direct_map_base(void);
    u64 direct_base = vmm_direct_map_base();
    int ret = blk_write_cached(swap_dev, lba, SECTORS_PER_PAGE, (const void *)(usize)(physical_frame + direct_base));
    if (ret < 0) {
        swap_free_slot(slot);
        return -1;
    }

    swap_table[slot].virtual_addr = virtual_addr;
    swap_table[slot].pml4_phys = pml4_phys;

    // Flush to ensure data is on disk
    blk_cache_flush(swap_dev);

    return (int)slot;
}

int swap_in(u64 pml4_phys, u64 virtual_addr, u64 *out_physical_frame)
{
    if (!swap_dev || !swap_dev->read_blocks) {
        return -1;
    }

    int slot = swap_find_slot(pml4_phys, virtual_addr);
    if (slot < 0) {
        return -1; // Not swapped
    }

    u64 lba = swap_start_lba + (u64)slot * SECTORS_PER_PAGE;

    // Allocate a new physical frame
    u64 frame = pmm_alloc_frame();
    if (!frame) {
        return -1;
    }

    // Read the page from swap
    extern u64 vmm_direct_map_base(void);
    u64 direct_base = vmm_direct_map_base();
    int ret = blk_read_cached(swap_dev, lba, SECTORS_PER_PAGE, (void *)(usize)(frame + direct_base));
    if (ret < 0) {
        pmm_free_frame(frame);
        return -1;
    }

    *out_physical_frame = frame;
    swap_free_slot((u32)slot);
    return 0;
}

void swap_free_all_slots(u64 pml4_phys)
{
    for (usize i = 0; i < swap_slot_count; i++) {
        if (swap_table[i].used && swap_table[i].pml4_phys == pml4_phys) {
            swap_free_slot((u32)i);
        }
    }
}

