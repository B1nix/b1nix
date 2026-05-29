#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/console.h>

/* Evictable user pages tracked for swap reclaim (B3 audit). Sized to actual
 * RAM at eviction_init() time, allocated from the kernel heap, instead of a
 * fixed 65536-entry BSS array. ~32 bytes per entry. Capacity follows total
 * RAM: roughly one entry per two physical pages so we track ~50% of memory
 * (the realistic userspace working set), clamped to a ceiling so a giant box
 * doesn't burn an absurd amount of bookkeeping. */
#define USER_PAGES_MIN  4096
#define USER_PAGES_MAX  (1024 * 1024)  /* 1M entries ~ 32 MiB ring */

struct mapped_page {
    struct task *task;
    u64 vaddr;
    u64 frame;
    int used;
};

static struct mapped_page *page_ring = 0;
static usize g_user_pages = 0;
static usize clock_hand = 0;
static usize page_count = 0;

/* Initialised lazily on the first eviction_register_page call so we can size
 * the ring once the pmm knows the total usable RAM. */
static void eviction_lazy_init(void) {
    if (page_ring) return;
    extern u64 pmm_total_usable_memory(void);
    u64 total_frames = pmm_total_usable_memory() / PAGE_SIZE;
    usize want = (usize)(total_frames / 2);
    if (want < USER_PAGES_MIN) want = USER_PAGES_MIN;
    if (want > USER_PAGES_MAX) want = USER_PAGES_MAX;
    page_ring = kzalloc(want * sizeof(struct mapped_page));
    if (!page_ring) {
        /* OOM during init: shrink to the floor and try again. */
        want = USER_PAGES_MIN;
        page_ring = kzalloc(want * sizeof(struct mapped_page));
        if (!page_ring) {
            /* Swap eviction effectively disabled (the ring stays NULL and
             * register/unregister/scan all short-circuit on g_user_pages = 0). */
            return;
        }
    }
    g_user_pages = want;
}

void eviction_register_page(struct task *task, u64 vaddr, u64 frame) {
    if (!task) return;
    eviction_lazy_init();
    if (!page_ring) return;  /* allocation failed — eviction stays off */

    // Avoid duplicates
    for (usize i = 0; i < g_user_pages; i++) {
        if (page_ring[i].used && page_ring[i].task == task && page_ring[i].vaddr == vaddr) {
            page_ring[i].frame = frame;
            return;
        }
    }

    for (usize i = 0; i < g_user_pages; i++) {
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
    for (usize i = 0; i < g_user_pages; i++) {
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

    for (usize i = 0; i < g_user_pages * 2; i++) {
        usize idx = clock_hand;
        clock_hand = (clock_hand + 1) % g_user_pages;

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
    for (usize i = 0; i < g_user_pages; i++) {
        if (page_ring[i].used && page_ring[i].task == task) {
            page_ring[i].used = 0;
            page_count--;
        }
    }
}


