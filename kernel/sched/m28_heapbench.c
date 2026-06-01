/* M28 #4 — SMP heap-contention benchmark.
 *
 * Measures whether the single global heap_lock serialises kmalloc/kfree across
 * cores badly enough to justify a per-CPU magazine. Stealable CPU-bound workers
 * (the same SMP-safe path as smp_test.c) each run a fixed number of
 * kmalloc+kfree pairs and record the rdtsc cycles they took. We run two rounds:
 *
 *   round A: 1 worker   — uncontended per-op cost
 *   round B: N workers  — contended per-op cost (N = online CPUs)
 *
 * If heap_lock is the bottleneck, round B's average per-op cycles balloon
 * roughly linearly with N (every kmalloc waits behind the others on one lock).
 * If per-op stays flat, the global lock is NOT the contention point and a
 * per-CPU magazine would add fragmentation for little gain.
 *
 * Test-mode only. Reports (averaged over all ops across all workers in a round):
 *   M28-HEAPBENCH: 1cpu_per_op=<cycles>
 *   M28-HEAPBENCH: Ncpu_per_op=<cycles> n=<N>
 *   M28-HEAPBENCH: ratio_x100=<(Ncpu/1cpu)*100>   (100 = no contention, ~N*100 = full serialisation)
 *   M28-HEAPBENCH: ok
 */

#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>

#define HB_MAX_WORKERS 8
#define HB_OPS_PER_WORKER 20000u

static inline u64 hb_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static spinlock_t hb_lock = SPINLOCK_INIT;
static volatile int hb_done;
static volatile u64 hb_total_cycles;
static volatile u64 hb_total_ops;
/* Workers spin on this until the launcher has queued the whole batch, so all
 * cores hammer the heap simultaneously (otherwise an early worker finishes
 * before the rest are even stolen and we never measure real contention). */
static volatile int hb_go;

static void hb_worker(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&hb_go, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");

    /* Mix of small size classes — the common kmalloc traffic (task structs,
     * vfs nodes, fd tables, path buffers). Each iteration allocs and frees so
     * the working set stays bounded; the cost measured is the lock + list
     * churn, not heap growth. */
    static const usize sizes[4] = {24, 64, 200, 512};
    u64 t0 = hb_rdtsc();
    for (u32 i = 0; i < HB_OPS_PER_WORKER; i++) {
        void *p = kmalloc(sizes[i & 3]);
        if (p) kfree(p);
    }
    u64 dt = hb_rdtsc() - t0;

    spin_lock(&hb_lock);
    hb_total_cycles += dt;
    hb_total_ops += HB_OPS_PER_WORKER;
    hb_done++;
    spin_unlock(&hb_lock);
}

/* Run one round with `nworkers` stealable workers; returns average cycles per
 * kmalloc+kfree op across all of them (0 on failure). The BSP busy-waits for
 * completion exactly like smp_selftest_run (it must not yield — only APs run
 * stealable workers). */
static u64 hb_run_round(int nworkers) {
    hb_done = 0;
    hb_total_cycles = 0;
    hb_total_ops = 0;
    __atomic_store_n(&hb_go, 0, __ATOMIC_RELEASE);

    int created = 0;
    for (int i = 0; i < nworkers; i++) {
        if (sched_create_stealable_worker("hbw", hb_worker, (void *)(usize)i) >= 0)
            created++;
    }
    if (created == 0)
        return 0;

    /* Release all workers at once. */
    __atomic_store_n(&hb_go, 1, __ATOMIC_RELEASE);

    u64 spins = 0;
    const u64 max_spins = 2000000ULL;
    int done = 0;
    do {
        spin_lock(&hb_lock);
        done = hb_done;
        spin_unlock(&hb_lock);
        for (volatile int k = 0; k < 20000; k++)
            __asm__ volatile("pause");
        spins++;
    } while (done < created && spins < max_spins);

    if (done < created)
        return 0; /* timed out — don't report a bogus number */

    u64 ops = hb_total_ops;
    return ops ? (hb_total_cycles / ops) : 0;
}

void m28_heapbench_run(void) {
    if (!bootinfo_has_flag("b1nix.test=1"))
        return;

    int cpus = get_online_cpu_count();
    if (cpus <= 1) {
        console_write("M28-HEAPBENCH: skip single-cpu\n");
        return;
    }
    if (cpus > HB_MAX_WORKERS)
        cpus = HB_MAX_WORKERS;

    console_write("M28-HEAPBENCH: start\n");

    u64 one = hb_run_round(1);
    u64 many = hb_run_round(cpus);

    console_write("M28-HEAPBENCH: 1cpu_per_op=");
    console_write_dec((u32)one);
    console_write("\n");
    console_write("M28-HEAPBENCH: Ncpu_per_op=");
    console_write_dec((u32)many);
    console_write(" n=");
    console_write_dec((u32)cpus);
    console_write("\n");
    if (one > 0) {
        console_write("M28-HEAPBENCH: ratio_x100=");
        console_write_dec((u32)((many * 100) / one));
        console_write("\n");
    }
    console_write("M28-HEAPBENCH: ok\n");
}
