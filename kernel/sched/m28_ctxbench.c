/* M28 #9 — context-switch and syscall-handler latency benchmark.
 *
 * Single-CPU only. The earlier SMP-4 prototype hit a stack-corruption
 * race the moment the iteration count crossed ~1000 (same shape #2 the
 * M14 iretq fault took, before the stack_released lease landed); even
 * with the lease fixed, SMP at this syscall density is still
 * unstable at this syscall density. Single CPU
 * is unaffected and gives the only baseline numbers worth comparing
 * against any future T4 work.
 *
 * Numbers reported (rdtsc cycles, average over N iterations):
 *   M28-BENCH: getpid_cycles=<n>    — light kernel function-call cost
 *   M28-BENCH: yield_cycles=<n>     — full ctx-switch cost (yield-back-to-self)
 *
 * Test-mode only — the smoke harness greps M28-BENCH: ok to verify the
 * benchmark ran without crashing. The cycle numbers themselves are
 * documentation, not pass/fail gates (they vary with host clock).
 */

#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/sched.h>

static inline u64 rdtsc(void) {
#if defined(__x86_64__)
    u32 lo, hi;
    { u64 c_ = arch_cycles(); lo = (u32)c_; hi = (u32)(c_ >> 32); }
    return ((u64)hi << 32) | lo;
#elif defined(__aarch64__)
    u64 val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    return 0;
#endif
}

void m28_ctxbench_run(void) {
    if (!bootinfo_has_flag("b1nix.test=1"))
        return;

    /* Skip SMP — the high-syscall-density race is real, see file header. */
    int cpus = get_online_cpu_count();
    if (cpus > 1) {
        console_write("M28-BENCH: skip smp (cpus=");
        console_write_dec(cpus);
        console_write(")\n");
        return;
    }

    console_write("M28-BENCH: start\n");

    /* Warm caches + IPI/lapic state. */
    for (int i = 0; i < 100; i++) (void)scheduler_get_pid();

    const u64 N = 1000;

    /* Light syscall-handler baseline: scheduler_get_pid is the SYS_GETPID
     * implementation, called here without the syscall-entry asm wrapper so
     * we measure only the kernel-internal cost. The asm wrapper adds a
     * fixed ~bkl_lock+frame-build overhead the full-syscall path would
     * inherit. */
    u64 t0 = rdtsc();
    for (u64 i = 0; i < N; i++) (void)scheduler_get_pid();
    u64 t1 = rdtsc();
    u64 getpid_cycles = (t1 - t0) / N;

    /* Full ctx-switch cost: scheduler_yield with only the boot task ready
     * yields back to itself, exercising the save/pick/restore path even
     * though the from/to task is identical. */
    t0 = rdtsc();
    for (u64 i = 0; i < N; i++) scheduler_yield();
    t1 = rdtsc();
    u64 yield_cycles = (t1 - t0) / N;

    console_write("M28-BENCH: getpid_cycles=");
    console_write_dec((u32)getpid_cycles);
    console_write("\n");
    console_write("M28-BENCH: yield_cycles=");
    console_write_dec((u32)yield_cycles);
    console_write("\n");
    console_write("M28-BENCH: ok\n");
}
