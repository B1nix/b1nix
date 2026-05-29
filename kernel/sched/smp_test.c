/* M24b SMP work-stealing self-test.
 *
 * Creates a batch of self-contained CPU-bound kernel workers (struct
 * task::stealable) on the BSP runqueue and proves that idle Application
 * Processors steal and execute them on other cores. The workers touch only
 * the test lock + counters and get_percpu(), so they are SMP-safe even though
 * the kernel's general syscall/VFS paths are not yet safe for parallel
 * kernel-mode execution (which is why ordinary tasks are never stealable).
 *
 * Verified by markers in the serial log (grepped by smoke_run/qrun-smp.sh):
 *   M24B-SMP: start
 *   M24B-SMP: cpus=<n>
 *   M24B-SMP: workers=<k>
 *   M24B-SMP: completed=<k>
 *   M24B-SMP: migrated=<m>          (workers that ran on a CPU != BSP)
 *   M24B-SMP: ok work-stealing      (completed==workers && migrated>0)
 */

#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/runqueue.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>

#define SMP_TEST_WORKERS 12

static spinlock_t g_smp_lock = SPINLOCK_INIT;
static volatile int g_smp_done;
static int g_smp_cpu_of[SMP_TEST_WORKERS];

/* CPU-bound worker payload. Runs (with interrupts disabled) on whichever CPU
 * steals it. Records the executing CPU id and bumps the completion counter
 * under the test lock — the lock both serialises the shared writes and acts as
 * the verification that mutual exclusion across cores actually holds. */
static void smp_worker(void *arg) {
    int idx = (int)(usize)arg;

    /* Enough arithmetic that the worker occupies its CPU long enough for
     * migration to be observable. Touches only a local. */
    volatile u64 acc = 0;
    for (u64 i = 0; i < 1000000ULL; i++)
        acc += i ^ (u64)idx;
    (void)acc;

    struct percpu *p = get_percpu();
    int cpu = p ? (int)p->cpu_id : -1;

    spin_lock(&g_smp_lock);
    if (idx >= 0 && idx < SMP_TEST_WORKERS)
        g_smp_cpu_of[idx] = cpu;
    g_smp_done++;
    spin_unlock(&g_smp_lock);
}

static void print_int(const char *prefix, int v) {
    console_write(prefix);
    console_write_dec(v);
    console_write("\n");
}

void smp_selftest_run(void) {
    if (!bootinfo_has_flag("b1nix.test=1"))
        return;

    /* APs flip their cpu_online flag inside ap_main, which may lag the BSP's
     * return from smp_boot_aps (that only waits on the trampoline ready flag).
     * Poll briefly so we don't mistake a still-booting AP for a single-CPU
     * system. */
    int cpus = get_online_cpu_count();
    for (u64 w = 0; cpus <= 1 && w < 400; w++) {
        for (volatile int k = 0; k < 100000; k++)
            __asm__ volatile("pause");
        cpus = get_online_cpu_count();
    }
    if (cpus <= 1) {
        /* No other core came online — work-stealing is genuinely N/A. */
        console_write("M24B-SMP: skip single-cpu\n");
        return;
    }

    console_write("M24B-SMP: start\n");
    print_int("M24B-SMP: cpus=", cpus);

    g_smp_done = 0;
    for (int i = 0; i < SMP_TEST_WORKERS; i++)
        g_smp_cpu_of[i] = -1;

    int created = 0;
    for (int i = 0; i < SMP_TEST_WORKERS; i++) {
        if (sched_create_stealable_worker("smpw", smp_worker,
                                          (void *)(usize)i) >= 0)
            created++;
    }
    print_int("M24B-SMP: workers=", created);

    /* Busy-wait until every created worker reports completion. The BSP must NOT
     * yield here: its cooperative scheduler would otherwise try to run a
     * stealable worker itself, but only the AP path (ap_main /
     * ap_worker_trampoline) knows how to execute and park one. */
    u64 spins = 0;
    const u64 max_spins = 200000ULL; /* generous; APs finish far sooner */
    int done = 0;
    do {
        spin_lock(&g_smp_lock);
        done = g_smp_done;
        spin_unlock(&g_smp_lock);
        for (volatile int k = 0; k < 20000; k++)
            __asm__ volatile("pause");
        spins++;
    } while (done < created && spins < max_spins);

    /* Safety net: drain any worker that was never stolen (only possible on
     * timeout) so the BSP scheduler never trips over a stealable task once
     * normal userspace scheduling begins. rq_dequeue is lock-protected, so an
     * AP mid-steal cannot race us into a double-reap. */
    {
        struct percpu *self = get_percpu();
        if (self) {
            for (;;) {
                interrupts_disable();
                struct task *leftover = rq_dequeue(&self->runqueue);
                interrupts_enable();
                if (!leftover)
                    break;
                if (leftover->stealable && leftover->state == TASK_READY) {
                    sched_ap_reap_worker(leftover);
                } else {
                    interrupts_disable();
                    rq_enqueue(&self->runqueue, leftover);
                    interrupts_enable();
                    break;
                }
            }
        }
    }

    int migrated = 0;
    for (int i = 0; i < created; i++)
        if (g_smp_cpu_of[i] > 0)
            migrated++;

    print_int("M24B-SMP: completed=", done);
    print_int("M24B-SMP: migrated=", migrated);

    if (created > 0 && done == created && migrated > 0)
        console_write("M24B-SMP: ok work-stealing\n");
    else
        console_write("M24B-SMP: fail work-stealing\n");
}
