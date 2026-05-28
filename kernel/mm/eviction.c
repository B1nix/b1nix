#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/console.h>

#define MAX_USER_PAGES 4096

struct mapped_page {
    struct task *task;
    u64 vaddr;
    u64 frame;
    int used;
};

static struct mapped_page page_ring[MAX_USER_PAGES];
static usize clock_hand = 0;
static usize page_count = 0;

void eviction_register_page(struct task *task, u64 vaddr, u64 frame) {
    if (!task) return;
    
    // Avoid duplicates
    for (usize i = 0; i < MAX_USER_PAGES; i++) {
        if (page_ring[i].used && page_ring[i].task == task && page_ring[i].vaddr == vaddr) {
            page_ring[i].frame = frame;
            return;
        }
    }

    for (usize i = 0; i < MAX_USER_PAGES; i++) {
        if (!page_ring[i].used) {
            page_ring[i].task = task;
            page_ring[i].vaddr = vaddr;
            page_ring[i].frame = frame;
            page_ring[i].used = 1;
            page_count++;
            return;
        }
    }
}

void eviction_unregister_page(u64 frame) {
    for (usize i = 0; i < MAX_USER_PAGES; i++) {
        if (page_ring[i].used && page_ring[i].frame == frame) {
            page_ring[i].used = 0;
            page_count--;
            return;
        }
    }
}

// Helper to get PTE accessed bit
static int is_page_accessed(struct task *task, u64 vaddr) {
    extern int paging_test_and_clear_accessed(u64 pml4_phys, u64 vaddr);
    if (!task) return 0;
    return paging_test_and_clear_accessed(task->pml4_phys, vaddr);
}

u64 swap_evict_page(void) {
    if (page_count == 0) return 0;

    for (usize i = 0; i < MAX_USER_PAGES * 2; i++) {
        usize idx = clock_hand;
        clock_hand = (clock_hand + 1) % MAX_USER_PAGES;

        if (!page_ring[idx].used) continue;

        struct task *t = page_ring[idx].task;
        u64 v = page_ring[idx].vaddr;
        u64 f = page_ring[idx].frame;

        // Second Chance check
        if (is_page_accessed(t, v)) {
            continue; // Give second chance and move to next
        }

        // Evict!
        if (swap_out(t->pml4_phys, v, f) >= 0) {
            extern void paging_mark_swapped(u64 pml4_phys, u64 vaddr);
            paging_mark_swapped(t->pml4_phys, v);
            
            page_ring[idx].used = 0;
            page_count--;
            
            // We don't call pmm_free_frame(f) here because we want to reuse it immediately
            return f;
        }
    }

    return 0;
}

void eviction_evict_page(void) {
    u64 frame = swap_evict_page();
    if (frame) {
        pmm_free_frame(frame);
    }
}

void eviction_unregister_all_pages(struct task *task) {
    if (!task) return;
    for (usize i = 0; i < MAX_USER_PAGES; i++) {
        if (page_ring[i].used && page_ring[i].task == task) {
            page_ring[i].used = 0;
            page_count--;
        }
    }
}


