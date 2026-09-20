/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/cgroup.h>
#include <b1nix/lapic.h>
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
    /* Chain in hash_tk: every page one task has mapped. The CLOCK hand does
     * not need it -- it walks the ring in order -- but reclaim inside a cgroup
     * does: it must find that cgroup's pages without looking at everybody
     * else's, and the ring holds an entry per two pages of RAM. Walking all of
     * them per crossing of a memory.max cost a page-table walk per candidate
     * and left the machine silent for a minute at a time. */
    u32 next_tk;
};

static struct mapped_page *page_ring = 0;
static usize g_user_pages = 0;
static usize clock_hand = 0;
static usize page_count = 0;
static u32 *hash_va = 0;
static u32 *hash_fr = 0;
static u32 *hash_tk = 0;
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

static inline usize ev_hash_tk(struct task *t) {
    u64 h = ((u64)(usize)t >> 4) * 0x9e3779b97f4a7c15ull;

    return (usize)((h >> 32) & hash_mask);
}

/* Which of the three chains a link belongs to. */
enum ev_chain { EV_CH_VA, EV_CH_FR, EV_CH_TK };

static inline u32 *ev_next(u32 idx, enum ev_chain ch) {
    return ch == EV_CH_VA   ? &page_ring[idx].next_va
           : ch == EV_CH_FR ? &page_ring[idx].next_fr
                            : &page_ring[idx].next_tk;
}

static void ev_chain_remove(u32 *head, u32 idx, enum ev_chain ch) {
    u32 *link = head;

    while (*link != EV_NIL) {
        u32 cur = *link;

        if (cur == idx) {
            *link = *ev_next(cur, ch);
            return;
        }
        link = ev_next(cur, ch);
    }
}

/* Drop slot `idx` from both indexes and return it to the free list. Caller
 * holds eviction_lock and has checked that the slot is in use. */
static void ev_slot_release(u32 idx) {
    ev_chain_remove(&hash_va[ev_hash_va(page_ring[idx].task, page_ring[idx].vaddr)], idx, EV_CH_VA);
    ev_chain_remove(&hash_fr[ev_hash_fr(page_ring[idx].frame)], idx, EV_CH_FR);
    ev_chain_remove(&hash_tk[ev_hash_tk(page_ring[idx].task)], idx, EV_CH_TK);
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
    u32 *htk = hfr ? kzalloc(buckets * sizeof(u32)) : 0;

    if (!htk) {
        /* OOM during init: fall back to the floor, and if even that fails
         * leave the ring NULL — register/unregister/scan all short-circuit on
         * it, so swap eviction is simply off. */
        if (hfr) kfree(hfr);
        if (hva) kfree(hva);
        if (ring) kfree(ring);
        want = USER_PAGES_MIN;
        buckets = 1;
        while (buckets < want) buckets <<= 1;
        ring = kzalloc(want * sizeof(struct mapped_page));
        hva = ring ? kzalloc(buckets * sizeof(u32)) : 0;
        hfr = hva ? kzalloc(buckets * sizeof(u32)) : 0;
        htk = hfr ? kzalloc(buckets * sizeof(u32)) : 0;
        if (!htk) {
            if (hfr) kfree(hfr);
            if (hva) kfree(hva);
            if (ring) kfree(ring);
            __atomic_store_n(&ev_init_started, 0, __ATOMIC_SEQ_CST);
            return;
        }
    }

    for (usize i = 0; i < buckets; i++) {
        hva[i] = EV_NIL;
        hfr[i] = EV_NIL;
        htk[i] = EV_NIL;
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
    hash_tk = htk;
    g_user_pages = want;
    /* Published last: every reader gates on page_ring. */
    __atomic_store_n(&page_ring, ring, __ATOMIC_RELEASE);
}

/* No swap device → no PT entry can ever become VMM_SWAPPED → the ring is
 * dead weight. Worse, every user-page map called eviction_register_page,
 * which did TWO linear scans over a ring sized as total_frames/2. At 8 GiB
 * RAM that was 2M comparisons per vmm_map_page; a 10 MB compiler binary load
 * (~2500 pages) burned ~5G comparisons just for the registration scans, so
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
            ev_chain_remove(&hash_fr[ev_hash_fr(page_ring[i].frame)], i, EV_CH_FR);
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
    usize btk = ev_hash_tk(task);

    free_head = page_ring[idx].next_va;
    page_ring[idx].task = task;
    page_ring[idx].vaddr = vaddr;
    page_ring[idx].frame = frame;
    page_ring[idx].used = 1;
    page_ring[idx].next_va = hash_va[bva];
    hash_va[bva] = idx;
    page_ring[idx].next_fr = hash_fr[bfr];
    hash_fr[bfr] = idx;
    page_ring[idx].next_tk = hash_tk[btk];
    hash_tk[btk] = idx;
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

/* Eviction must not re-enter itself on the same CPU.
 *
 * Writing a page out allocates -- zswap takes a blob for the compressed copy,
 * zram takes one per stored page -- and an allocation can grow the kernel
 * heap, and growing it asks the page allocator for frames, and the page
 * allocator under pressure reclaims, which lands back here. Each turn of that
 * wheel is deeper than the last with no bound on it: the machine spends
 * minutes inside one allocation and everything else waiting on the heap lock
 * eventually calls it a lockup, which is exactly what a btrfs transaction
 * thread reported.
 *
 * So one eviction per CPU at a time. A nested attempt simply declines, and its
 * caller does what it does when there is nothing to reclaim -- allocates
 * anyway, or kills. Per CPU rather than one global flag so that reclaim on one
 * core does not silently turn another core's memory.max into a kill. */
static volatile int ev_busy[MAX_CPUS];

static int ev_reentry_begin(void) {
	struct percpu *p = get_percpu();
	unsigned cpu = p ? (unsigned)p->cpu_id : 0;

	if (cpu >= MAX_CPUS)
		cpu = 0;
	if (__atomic_exchange_n(&ev_busy[cpu], 1, __ATOMIC_ACQ_REL))
		return -1;
	return (int)cpu;
}

static void ev_reentry_end(int cpu) {
	if (cpu >= 0)
		__atomic_store_n(&ev_busy[cpu], 0, __ATOMIC_RELEASE);
}

/* The page the CPU is about to touch, and must therefore not lose.
 *
 * Reclaim inside a cgroup runs from the page fault that took the cgroup over
 * its limit -- which means the page the fault just installed is a candidate
 * for the very scan the fault started, and it is the coldest-looking page
 * there is: nothing has read or written it yet, so its accessed bit is clear.
 * Evicting it sends the faulting instruction straight back into a fault, which
 * charges the cgroup again, which reclaims again, which takes the page again.
 * The machine makes no forward progress and says nothing while it does it.
 *
 * So the fault marks its own page for the duration of the charge. One slot per
 * CPU, because that is how many faults a CPU is inside at once. */
static volatile struct {
	struct task *task;
	u64 vaddr;
} ev_protected[MAX_CPUS];

static unsigned ev_this_cpu(void) {
	struct percpu *p = get_percpu();
	unsigned cpu = p ? (unsigned)p->cpu_id : 0;

	return cpu < MAX_CPUS ? cpu : 0;
}

void eviction_protect_begin(struct task *t, u64 vaddr) {
	unsigned cpu = ev_this_cpu();

	ev_protected[cpu].task = t;
	ev_protected[cpu].vaddr = vaddr & ~(u64)(PAGE_SIZE - 1);
}

void eviction_protect_end(void) {
	unsigned cpu = ev_this_cpu();

	ev_protected[cpu].task = 0;
	ev_protected[cpu].vaddr = 0;
}

static int ev_is_protected(struct task *t, u64 vaddr) {
	for (unsigned i = 0; i < MAX_CPUS; i++)
		if (ev_protected[i].task == t && ev_protected[i].vaddr == vaddr)
			return 1;
	return 0;
}

/* Try to evict the page the hand is on. Returns 1 and the frame it freed up,
 * 0 if this page was not a candidate (wrong owner, locked, recently used, or
 * its cgroup is at its memory.swap.max). The frame is NOT returned to the PMM:
 * the caller decides, because the machine-wide path wants to reuse it directly
 * and the cgroup path wants it back in the pool. */
static int ev_try_evict(usize idx, struct task *only, usize *scanned,
                        u64 *frame_out) {
    u64 flags;
    struct task *t;
    u64 v, f;

    spin_lock_irqsave(&eviction_lock, &flags);
    if (!page_ring[idx].used) {
        spin_unlock_irqrestore(&eviction_lock, flags);
        return 0;
    }
    t = page_ring[idx].task;
    v = page_ring[idx].vaddr;
    f = page_ring[idx].frame;
    spin_unlock_irqrestore(&eviction_lock, flags);

    /* A targeted reclaim takes one task's pages and nobody else's; the entry
     * may have been reused for another task since it was collected. */
    if (only && t != only)
        return 0;

    if (scanned)
        (*scanned)++;

    /* mlock(2): the owner asked for this page to stay resident. */
    if (page_is_locked(t, v))
        return 0;

    /* The page a fault is in the middle of installing. See ev_protected. */
    if (ev_is_protected(t, v))
        return 0;

    /* Second chance: a page touched since the last sweep is still in use. */
    if (is_page_accessed(t, v))
        return 0;

    /* memory.swap.max: a cgroup at its swap limit does not get to put another
     * page out. Charged before the write rather than after it, so the limit is
     * decided once, by the cgroup that owns the page, and not by whoever
     * happens to free the slot later. */
    u16 owner = cgroup_id_of_task(t ? t->id : 0);

    if (cgroup_swap_charge(owner, 1) != 0)
        return 0;

    /* swap_out returns the slot index; we encode it into the PTE so the #PF
     * handler can swap the page back in without any reverse map. Done outside
     * the lock: it is block I/O and can sleep. */
    int swslot = swap_out(f);

    if (swslot < 0) {
        /* The page did not go out after all -- give the charge back, or the
         * cgroup would be billed for a page that is still in RAM. */
        cgroup_swap_uncharge(owner, 1);
        return 0;
    }

    extern int paging_mark_swapped(u64 pml4_phys, u64 vaddr, u64 slot);
    /* The slot carries the charge from here on. */
    swap_set_owner((u32)swslot, owner);
    if (!paging_mark_swapped(t->pml4_phys, v, (u64)swslot)) {
        /* Nothing was mapped there any more: the page was unmapped while the
         * write was in flight. The slot now points at data no page table
         * names, so give it back -- which also returns the charge -- and drop
         * the stale ring entry. Leaving it would leak a swap slot and a
         * cgroup's memory.swap.current for the life of the machine. */
        swap_free_slot_index((u32)swslot);
        spin_lock_irqsave(&eviction_lock, &flags);
        if (page_ring[idx].used && page_ring[idx].frame == f &&
            page_ring[idx].task == t && page_ring[idx].vaddr == v)
            ev_slot_release((u32)idx);
        spin_unlock_irqrestore(&eviction_lock, flags);
        return 0;
    }

    /* paging_mark_swapped only invlpg's the CURRENT CPU, but the evicted page
     * belongs to task t, which may be running (or have threads sharing its
     * address space) on ANOTHER CPU whose TLB still maps v -> f. Without a
     * cross-CPU shootdown that stale entry lets the other CPU write into the
     * frame after we free/reuse it — a use-after-free that corrupts the PMM
     * free-list (GP fault in freelist_pop). Shoot v down on all CPUs before the
     * frame is reused. No-op on a single CPU. */
    extern void tlb_shootdown_page(u64 vaddr);
    tlb_shootdown_page(v);

    spin_lock_irqsave(&eviction_lock, &flags);
    /* The slot may have been unregistered and reused while the write was in
     * flight; only release it if it still describes this page. */
    if (page_ring[idx].used && page_ring[idx].frame == f &&
        page_ring[idx].task == t && page_ring[idx].vaddr == v)
        ev_slot_release((u32)idx);
    spin_unlock_irqrestore(&eviction_lock, flags);

    *frame_out = f;
    return 1;
}

/* Advance the hand one step and say where it was. */
static usize ev_hand_step(void) {
    u64 flags;
    usize idx;

    spin_lock_irqsave(&eviction_lock, &flags);
    idx = clock_hand;
    clock_hand = (clock_hand + 1) % g_user_pages;
    spin_unlock_irqrestore(&eviction_lock, flags);
    return idx;
}

u64 swap_evict_page(void) {
    if (!page_ring || page_count == 0) return 0;

    int cpu = ev_reentry_begin();

    if (cpu < 0)
        return 0;   /* already evicting on this CPU: see ev_reentry_begin */

    u64 got = 0;

    for (usize i = 0; i < g_user_pages * 2; i++) {
        u64 frame = 0;

        if (ev_try_evict(ev_hand_step(), 0, 0, &frame)) {
            got = frame;   /* the caller is allocating: it wants this frame */
            break;
        }
    }
    ev_reentry_end(cpu);
    return got;
}

/* Reclaim from ONE task's pages.
 *
 * The CLOCK hand is the wrong instrument for this. It walks the ring in order
 * -- an entry per two pages of RAM -- and a cgroup's pages are a small part of
 * it, so finding them that way costs a sweep of everybody's memory each time a
 * cgroup crosses its limit, inside the fault that crossed it. On a machine
 * with a gigabyte of RAM that is a quarter of a million ring steps and tens of
 * thousands of page-table walks per crossing; the guest stopped answering for
 * a minute at a time. The per-task chain makes the cost proportional to what
 * is actually being reclaimed.
 *
 * Two passes, for the same reason the hand takes two: the first clears the
 * accessed bit of a page that has been touched, the second may take it. A
 * cgroup that has just filled its memory has nothing BUT recently touched
 * pages, and one pass over them reclaims nothing at all.
 */
usize eviction_reclaim_task(struct task *t, usize want_pages, usize *scanned) {
    if (!page_ring || !t || !want_pages)
        return 0;

    int cpu = ev_reentry_begin();

    if (cpu < 0)
        return 0;   /* already evicting on this CPU: see ev_reentry_begin */

    /* Indices are copied out under the lock and processed without it: evicting
     * a page unlinks it from this very chain, and swap_out may block. */
    enum { EV_BATCH = 64 };
    u32 batch[EV_BATCH];
    usize freed = 0;

    for (int pass = 0; pass < 2 && freed < want_pages; pass++) {
        u32 cursor = EV_NIL;
        int first = 1;

        for (;;) {
            u64 flags;
            usize n = 0;

            spin_lock_irqsave(&eviction_lock, &flags);
            u32 i = first ? hash_tk[ev_hash_tk(t)] : cursor;

            first = 0;
            while (i != EV_NIL && n < EV_BATCH) {
                if (page_ring[i].used && page_ring[i].task == t)
                    batch[n++] = i;
                i = page_ring[i].next_tk;
            }
            cursor = i;
            spin_unlock_irqrestore(&eviction_lock, flags);

            for (usize k = 0; k < n && freed < want_pages; k++) {
                u64 frame = 0;

                if (ev_try_evict(batch[k], t, scanned, &frame)) {
                    pmm_free_frame(frame);
                    freed++;
                }
            }
            if (cursor == EV_NIL || freed >= want_pages)
                break;
        }
    }
    ev_reentry_end(cpu);
    return freed;
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
