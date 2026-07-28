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

/* ── mlock(2) support ──────────────────────────────────────────────────────
 * A locked range is memory the owning task asked to keep resident. The CLOCK
 * scan below skips any page that falls inside one, so mlock/mlockall are a
 * real guarantee (the page is never handed to swap_out) rather than a
 * success-returning no-op. Ranges live in a small side table keyed by task —
 * struct task must not grow (per-task fields there fault the LAPIC page
 * tables), and a handful of ranges covers every real caller (a daemon locking
 * its whole address space, or one or two buffers). */
#define EVICTION_MAX_LOCKED 32

struct locked_range {
    struct task *task;
    u64 start; /* inclusive, page-aligned */
    u64 end;   /* exclusive, page-aligned */
    int used;
};

static struct locked_range locked_ranges[EVICTION_MAX_LOCKED];

/* Is `vaddr` inside a range `task` locked? */
static int page_is_locked(struct task *task, u64 vaddr) {
    for (usize i = 0; i < EVICTION_MAX_LOCKED; i++) {
        if (!locked_ranges[i].used || locked_ranges[i].task != task)
            continue;
        if (vaddr >= locked_ranges[i].start && vaddr < locked_ranges[i].end)
            return 1;
    }
    return 0;
}

int eviction_lock_range(struct task *task, u64 start, u64 end) {
    if (!task || end <= start) return -1;
    start &= ~(u64)(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);

    /* Extend an adjacent/overlapping range of the same task instead of burning
     * a slot per call (musl's mlock loop over a heap arena would otherwise
     * exhaust the table). */
    for (usize i = 0; i < EVICTION_MAX_LOCKED; i++) {
        if (!locked_ranges[i].used || locked_ranges[i].task != task)
            continue;
        if (start <= locked_ranges[i].end && end >= locked_ranges[i].start) {
            if (start < locked_ranges[i].start) locked_ranges[i].start = start;
            if (end > locked_ranges[i].end) locked_ranges[i].end = end;
            return 0;
        }
    }
    for (usize i = 0; i < EVICTION_MAX_LOCKED; i++) {
        if (locked_ranges[i].used) continue;
        locked_ranges[i].task = task;
        locked_ranges[i].start = start;
        locked_ranges[i].end = end;
        locked_ranges[i].used = 1;
        return 0;
    }
    return -1; /* table full -> caller reports ENOMEM, as Linux does */
}

void eviction_unlock_range(struct task *task, u64 start, u64 end) {
    if (!task || end <= start) return;
    start &= ~(u64)(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
    for (usize i = 0; i < EVICTION_MAX_LOCKED; i++) {
        if (!locked_ranges[i].used || locked_ranges[i].task != task)
            continue;
        struct locked_range *r = &locked_ranges[i];
        if (end <= r->start || start >= r->end)
            continue; /* disjoint */
        if (start <= r->start && end >= r->end) {
            r->used = 0; /* fully unlocked */
        } else if (start <= r->start) {
            r->start = end; /* trim the front */
        } else if (end >= r->end) {
            r->end = start; /* trim the back */
        } else {
            /* Punching a hole: keep the head here and record the tail. */
            u64 tail_start = end, tail_end = r->end;
            r->end = start;
            eviction_lock_range(task, tail_start, tail_end);
        }
    }
}

void eviction_unlock_all(struct task *task) {
    if (!task) return;
    for (usize i = 0; i < EVICTION_MAX_LOCKED; i++)
        if (locked_ranges[i].used && locked_ranges[i].task == task)
            locked_ranges[i].used = 0;
}

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

/* No swap device → no PT entry can ever become VMM_SWAPPED → the ring is
 * dead weight. Worse, every user-page map called eviction_register_page,
 * which did TWO linear scans over a ring sized as total_frames/2. At 8 GiB
 * RAM that was 2M comparisons per vmm_map_page; gcc binary load (10 MB =
 * ~2500 pages) burned ~5G comparisons just for the registration scans, so
 * smp=1/-j1 throughput fell ~÷16 from 512 MB to 8192 MB. Short-circuit
 * here so swap-less guests skip the bookkeeping entirely. */
extern int swap_active(void);

void eviction_register_page(struct task *task, u64 vaddr, u64 frame) {
    if (!task) return;
    if (!swap_active()) return;   /* no swap → no need to track */
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
    if (!page_ring) return;       /* ring never allocated (no swap) */
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

        /* mlock(2): the owner asked for this page to stay resident. */
        if (page_is_locked(t, v)) {
            continue;
        }

        // Second Chance check
        if (is_page_accessed(t, v)) {
            continue; // Give second chance and move to next
        }

        // Evict! swap_out returns the slot index; we encode it into the PTE so
        // the #PF handler can swap the page back in without any reverse map.
        int swslot = swap_out(f);
        if (swslot >= 0) {
            extern void paging_mark_swapped(u64 pml4_phys, u64 vaddr, u64 slot);
            paging_mark_swapped(t->pml4_phys, v, (u64)swslot);

            /* paging_mark_swapped only invlpg's the CURRENT CPU, but the evicted
             * page belongs to task t, which may be running (or have threads
             * sharing its address space) on ANOTHER CPU whose TLB still maps
             * v -> f. Without a cross-CPU shootdown that stale entry lets the
             * other CPU write into the frame after we free/reuse it — a
             * use-after-free that corrupts the PMM free-list (GP fault in
             * freelist_pop). Shoot v down on all CPUs before the frame is
             * reused. No-op on a single CPU. */
            extern void tlb_shootdown_page(u64 vaddr);
            tlb_shootdown_page(v);

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
    eviction_unlock_all(task);    /* mlock ranges die with the task */
    if (!page_ring) return;       /* ring never allocated (no swap) */
    for (usize i = 0; i < g_user_pages; i++) {
        if (page_ring[i].used && page_ring[i].task == task) {
            page_ring[i].used = 0;
            page_count--;
        }
    }
}


