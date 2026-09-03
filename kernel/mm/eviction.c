#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/console.h>
#include <b1nix/spinlock.h>

/* Evictable user pages tracked for swap reclaim (B3 audit). Sized to actual
 * RAM at eviction_init() time, allocated from the kernel heap, instead of a
 * fixed 65536-entry BSS array. ~32 bytes per entry. Capacity follows total
 * RAM: roughly one entry per two physical pages so we track ~50% of memory
 * (the realistic userspace working set), clamped to a ceiling so a giant box
 * doesn't burn an absurd amount of bookkeeping. */
#define USER_PAGES_MIN  4096
#define USER_PAGES_MAX  (1024 * 1024)  /* 1M entries ~ 32 MiB ring */

/* The ring is the CLOCK scan order, so an entry keeps its slot for life. Two
 * hash tables index it: by (task, vaddr) for the dedup on register, and by
 * frame for the unregister on unmap. Both were linear scans over a ring
 * holding one entry per two physical pages — at 1 GiB that is 131072 entries
 * walked TWICE for every user page mapped. An exec mapping a few thousand
 * pages therefore spent hundreds of milliseconds in bookkeeping alone, and
 * because the ring only exists when a swap device is attached, attaching one
 * made every process start about ten times slower. */
#define EV_NIL 0xffffffffu

struct mapped_page {
    struct task *task;
    u64 vaddr;
    u64 frame;
    int used;
    u32 next_va;  /* chain in hash_va, or the free-slot list when !used */
    u32 next_fr;  /* chain in hash_fr */
};

static struct mapped_page *page_ring = 0;
static usize g_user_pages = 0;
static usize clock_hand = 0;
static usize page_count = 0;
static u32 *hash_va = 0;
static u32 *hash_fr = 0;
static usize hash_mask = 0;
static u32 free_head = EV_NIL;

/* Guards the ring, the chains and the free list. Held only across index
 * surgery — never across swap_out(), which does block I/O. */
static spinlock_t eviction_lock;

static inline usize ev_hash_va(struct task *t, u64 vaddr) {
    u64 h = ((u64)(usize)t >> 4) ^ (vaddr >> 12);

    h *= 0x9e3779b97f4a7c15ull;
    return (usize)((h >> 32) & hash_mask);
}

static inline usize ev_hash_fr(u64 frame) {
    u64 h = (frame >> 12) * 0x9e3779b97f4a7c15ull;

    return (usize)((h >> 32) & hash_mask);
}

static void ev_chain_remove(u32 *head, u32 idx, int by_frame) {
    u32 *link = head;

    while (*link != EV_NIL) {
        u32 cur = *link;

        if (cur == idx) {
            *link = by_frame ? page_ring[cur].next_fr : page_ring[cur].next_va;
            return;
        }
        link = by_frame ? &page_ring[cur].next_fr : &page_ring[cur].next_va;
    }
}

/* Drop slot `idx` from both indexes and return it to the free list. Caller
 * holds eviction_lock and has checked that the slot is in use. */
static void ev_slot_release(u32 idx) {
    ev_chain_remove(&hash_va[ev_hash_va(page_ring[idx].task, page_ring[idx].vaddr)], idx, 0);
    ev_chain_remove(&hash_fr[ev_hash_fr(page_ring[idx].frame)], idx, 1);
    page_ring[idx].used = 0;
    page_ring[idx].next_va = free_head;
    free_head = idx;
    page_count--;
}

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
/* Set once by whichever CPU builds the ring; a loser simply leaves the page
 * untracked for now rather than allocating a second ring. */
static volatile int ev_init_started;

/* Called WITHOUT eviction_lock: kzalloc can grow the heap, and growing it can
 * reclaim and write back, neither of which may happen under a spinlock. */
static void eviction_lazy_init(void) {
    if (page_ring) return;
    if (__atomic_exchange_n(&ev_init_started, 1, __ATOMIC_SEQ_CST)) return;
    extern u64 pmm_total_usable_memory(void);
    u64 total_frames = pmm_total_usable_memory() / PAGE_SIZE;
    usize want = (usize)(total_frames / 2);
    if (want < USER_PAGES_MIN) want = USER_PAGES_MIN;
    if (want > USER_PAGES_MAX) want = USER_PAGES_MAX;

    usize buckets = 1;
    while (buckets < want) buckets <<= 1;

    struct mapped_page *ring = kzalloc(want * sizeof(struct mapped_page));
    u32 *hva = ring ? kzalloc(buckets * sizeof(u32)) : 0;
    u32 *hfr = hva ? kzalloc(buckets * sizeof(u32)) : 0;

    if (!hfr) {
        /* OOM during init: fall back to the floor, and if even that fails
         * leave the ring NULL — register/unregister/scan all short-circuit on
         * it, so swap eviction is simply off. */
        if (hva) kfree(hva);
        if (ring) kfree(ring);
        want = USER_PAGES_MIN;
        buckets = 1;
        while (buckets < want) buckets <<= 1;
        ring = kzalloc(want * sizeof(struct mapped_page));
        hva = ring ? kzalloc(buckets * sizeof(u32)) : 0;
        hfr = hva ? kzalloc(buckets * sizeof(u32)) : 0;
        if (!hfr) {
            if (hva) kfree(hva);
            if (ring) kfree(ring);
            __atomic_store_n(&ev_init_started, 0, __ATOMIC_SEQ_CST);
            return;
        }
    }

    for (usize i = 0; i < buckets; i++) {
        hva[i] = EV_NIL;
        hfr[i] = EV_NIL;
    }
    /* Thread every slot onto the free list, lowest index first. */
    for (usize i = want; i-- > 0;) {
        ring[i].next_va = (i + 1 < want) ? (u32)(i + 1) : EV_NIL;
        ring[i].next_fr = EV_NIL;
    }
    free_head = 0;
    hash_mask = buckets - 1;
    hash_va = hva;
    hash_fr = hfr;
    g_user_pages = want;
    /* Published last: every reader gates on page_ring. */
    __atomic_store_n(&page_ring, ring, __ATOMIC_RELEASE);
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

    u64 flags;

    if (!page_ring) {
        eviction_lazy_init();
        if (!page_ring) return;   /* not built yet — page stays untracked */
    }

    spin_lock_irqsave(&eviction_lock, &flags);

    usize bva = ev_hash_va(task, vaddr);

    for (u32 i = hash_va[bva]; i != EV_NIL; i = page_ring[i].next_va) {
        if (page_ring[i].task != task || page_ring[i].vaddr != vaddr)
            continue;
        if (page_ring[i].frame != frame) {
            /* Same virtual page, different frame (CoW, swap-in): re-key the
             * frame index, which is what the unmap path looks it up by. */
            ev_chain_remove(&hash_fr[ev_hash_fr(page_ring[i].frame)], i, 1);
            page_ring[i].frame = frame;
            page_ring[i].next_fr = hash_fr[ev_hash_fr(frame)];
            hash_fr[ev_hash_fr(frame)] = i;
        }
        spin_unlock_irqrestore(&eviction_lock, flags);
        return;
    }

    if (free_head == EV_NIL) {    /* ring full — this page just isn't tracked */
        spin_unlock_irqrestore(&eviction_lock, flags);
        return;
    }

    u32 idx = free_head;
    usize bfr = ev_hash_fr(frame);

    free_head = page_ring[idx].next_va;
    page_ring[idx].task = task;
    page_ring[idx].vaddr = vaddr;
    page_ring[idx].frame = frame;
    page_ring[idx].used = 1;
    page_ring[idx].next_va = hash_va[bva];
    hash_va[bva] = idx;
    page_ring[idx].next_fr = hash_fr[bfr];
    hash_fr[bfr] = idx;
    page_count++;
    spin_unlock_irqrestore(&eviction_lock, flags);
}

void eviction_unregister_page(u64 frame) {
    if (!page_ring) return;       /* ring never allocated (no swap) */

    u64 flags;

    spin_lock_irqsave(&eviction_lock, &flags);
    for (u32 i = hash_fr[ev_hash_fr(frame)]; i != EV_NIL; i = page_ring[i].next_fr) {
        if (page_ring[i].used && page_ring[i].frame == frame) {
            ev_slot_release(i);
            break;
        }
    }
    spin_unlock_irqrestore(&eviction_lock, flags);
}

// Helper to get PTE accessed bit
static int is_page_accessed(struct task *task, u64 vaddr) {
    extern int paging_test_and_clear_accessed(u64 pml4_phys, u64 vaddr);
    if (!task) return 0;
    return paging_test_and_clear_accessed(task->pml4_phys, vaddr);
}

u64 swap_evict_page(void) {
    if (!page_ring || page_count == 0) return 0;

    for (usize i = 0; i < g_user_pages * 2; i++) {
        u64 flags;
        struct task *t;
        u64 v, f;
        usize idx;

        spin_lock_irqsave(&eviction_lock, &flags);
        idx = clock_hand;
        clock_hand = (clock_hand + 1) % g_user_pages;
        if (!page_ring[idx].used) {
            spin_unlock_irqrestore(&eviction_lock, flags);
            continue;
        }
        t = page_ring[idx].task;
        v = page_ring[idx].vaddr;
        f = page_ring[idx].frame;
        spin_unlock_irqrestore(&eviction_lock, flags);

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
        // Done outside the lock: it is block I/O and can sleep.
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

            spin_lock_irqsave(&eviction_lock, &flags);
            /* The slot may have been unregistered and reused while the write
             * was in flight; only release it if it still describes this page. */
            if (page_ring[idx].used && page_ring[idx].frame == f &&
                page_ring[idx].task == t && page_ring[idx].vaddr == v)
                ev_slot_release((u32)idx);
            spin_unlock_irqrestore(&eviction_lock, flags);

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

    u64 flags;

    spin_lock_irqsave(&eviction_lock, &flags);
    for (usize i = 0; i < g_user_pages; i++)
        if (page_ring[i].used && page_ring[i].task == task)
            ev_slot_release((u32)i);
    spin_unlock_irqrestore(&eviction_lock, flags);
}
