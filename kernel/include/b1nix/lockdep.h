#ifndef B1NIX_LOCKDEP_H
#define B1NIX_LOCKDEP_H

#include <b1nix/types.h>

/* Lockdep-light: per-CPU acquisition-order tracker.
 *
 * Build with -DKERNEL_LOCKDEP=1 to enable. Default off so production builds
 * pay zero cost: every entry/exit point becomes a no-op the optimiser drops.
 *
 * The DAG (see docs/m28-locking.md) assigns each lock a monotonic level —
 * outer locks have small numbers, inner locks have large numbers. A thread
 * already holding level N may only acquire level M when M > N. Any attempt
 * to acquire at M <= N panics with the held-lock stack.
 *
 * Caveats:
 *  - Tracks ACQUISITION ORDER, not release order. Mismatched release is
 *    flagged separately (LIFO check).
 *  - Per-CPU stack depth is bounded at 16. Real held-lock depth in b1nix
 *    today peaks at ~5 (BKL + g_tasks_lock + per-CPU rq + per-inode rw +
 *    heap), so 16 is comfortable headroom.
 *  - Uses cpu_id (from get_percpu()) at runtime — must be called only
 *    after percpu_init has set the BSP GS base. Earliest-boot locks should
 *    skip lockdep with the _quiet variants.
 */

/* Lock levels — keep these in monotonic top-down order matching the DAG.
 * Add new levels in their proper slot; never reuse a number. */
typedef enum {
    LOCKDEP_LVL_BKL          = 100,  /* outermost — Big Kernel Lock */
    LOCKDEP_LVL_TASKS        = 200,  /* g_tasks_lock — process-table slots */
    LOCKDEP_LVL_FD           = 210,  /* per-task fd_lock — sibling of TASKS */
    LOCKDEP_LVL_MOUNT_TBL    = 300,  /* vfs_mount_lock, dcache, icache */
    LOCKDEP_LVL_INODE        = 400,  /* vfs_inode rw_lock (sleeping) */
    LOCKDEP_LVL_VFS_TREE     = 500,  /* vfs_tree_lock (M28-B) — chain walks */
    LOCKDEP_LVL_BCACHE       = 600,  /* block-cache hash + LRU */
    /* HEAP < VMM < PMM: kmalloc may call kheap_grow which calls vmm_map_page,
     * which in turn allocates intermediate page tables via pmm_alloc_frame.
     * So HEAP wraps VMM wraps PMM. */
    LOCKDEP_LVL_HEAP         = 700,  /* kheap general arena */
    LOCKDEP_LVL_VMM          = 750,  /* page-table mutations (M28 F2) */
    LOCKDEP_LVL_PMM          = 800,  /* physical-frame allocator */
    LOCKDEP_LVL_PAGECACHE    = 900,  /* page_cache_lock */
    /* RUNQUEUE is a TERMINAL leaf — acquired briefly by sched_rq_enqueue /
     * rq_dequeue, but those callers can be nested inside any sleeping lock
     * release path (vfs_inode_unlock_* -> scheduler_wake_all, plus every
     * yield-on-contention atomic-test-and-set lock). Putting it past every
     * outer-tier lock means "anywhere can wake/yield" — which is what we
     * actually do. */
    LOCKDEP_LVL_RUNQUEUE     = 1000,
} lockdep_level_t;

#ifdef KERNEL_LOCKDEP

/* Push a lock onto the calling CPU's acquisition stack. Panics if the new
 * level is <= the current top (DAG inversion) or if the stack overflows. */
void lockdep_acquire(int level, const char *name);

/* Pop the calling CPU's stack. Panics if the released level does not match
 * the top of the stack (out-of-order release / double release). */
void lockdep_release(int level);

/* Dump the current held-lock stack for `cpu` to console. Called from
 * panic() to surface lock state. Safe to call before percpu_init. */
void lockdep_dump_cpu(int cpu);

/* Dump every CPU's stack — used by panic. */
void lockdep_dump_all(void);

#define LOCKDEP_ACQUIRE(lvl)  lockdep_acquire((int)(lvl), #lvl)
#define LOCKDEP_RELEASE(lvl)  lockdep_release((int)(lvl))

#else  /* !KERNEL_LOCKDEP */

#define LOCKDEP_ACQUIRE(lvl)  ((void)0)
#define LOCKDEP_RELEASE(lvl)  ((void)0)

static inline void lockdep_dump_cpu(int cpu) { (void)cpu; }
static inline void lockdep_dump_all(void) {}

#endif  /* KERNEL_LOCKDEP */

#endif /* B1NIX_LOCKDEP_H */
