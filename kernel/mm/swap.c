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

#define MAX_SWAP_SLOTS 1024
#define SECTORS_PER_PAGE (PAGE_SIZE / 512)

static struct block_device *swap_dev = 0;
static u64 swap_start_lba = 0; // First LBA of swap area
static u64 swap_sector_count = 0;

// Swap slot table: maps virtual page -> swap location
static struct {
    u64 pml4_phys;         // Address space ID
    u64 virtual_addr;      // Page-aligned virtual address
    u32 slot_index;        // Index into swap area
    int used;
} swap_table[MAX_SWAP_SLOTS];

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
        console_write("swap: device=");
        console_write(dev->name);
        console_write(" start_lba=");
        console_write_dec(swap_start_lba);
        console_write(" sectors=");
        console_write_dec(swap_sector_count);
        console_write("\n");
    }
}

int swap_init(void)
{
    memset(swap_table, 0, sizeof(swap_table));
    console_write("swap: initialized, max_slots=");
    console_write_dec(MAX_SWAP_SLOTS);
    console_write("\n");

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
            int slot = swap_out(fake_vaddr, test_frame);
            if (slot >= 0) {
                console_write("swap: page swap-out ok, slot=");
                console_write_dec(slot);
                console_write("\n");

                memset(ptr, 0, PAGE_SIZE);

                u64 out_frame = 0;
                if (swap_in(fake_vaddr, &out_frame) == 0 && out_frame != 0) {
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
    for (usize i = 0; i < MAX_SWAP_SLOTS; i++) {
        u32 idx = (swap_next_slot + i) % MAX_SWAP_SLOTS;
        if (!swap_table[idx].used) {
            swap_table[idx].used = 1;
            swap_next_slot = (idx + 1) % MAX_SWAP_SLOTS;
            return idx;
        }
    }
    return (u32)-1; // No free slots
}

static void swap_free_slot(u32 slot_index)
{
    if (slot_index < MAX_SWAP_SLOTS) {
        swap_table[slot_index].used = 0;
        swap_table[slot_index].virtual_addr = 0;
        swap_table[slot_index].pml4_phys = 0;
    }
}

static int swap_find_slot(u64 pml4_phys, u64 virtual_addr)
{
    for (usize i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (swap_table[i].used && swap_table[i].pml4_phys == pml4_phys &&
            swap_table[i].virtual_addr == virtual_addr) {
            return (int)i;
        }
    }
    return -1;
}

int swap_out(u64 virtual_addr, u64 physical_frame)
{
    u64 pml4_phys = current_task ? current_task->pml4_phys : 0;

    if (!swap_dev || !swap_dev->write_blocks) {
        console_write("swap_out: no swap device\n");
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

int swap_in(u64 virtual_addr, u64 *out_physical_frame)
{
    u64 pml4_phys = current_task ? current_task->pml4_phys : 0;

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
