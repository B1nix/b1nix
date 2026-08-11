/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SMP_H
#define LKPI_LINUX_SMP_H
#include <lkpi/env.h>
#include <linux/types.h>
static inline int smp_processor_id(void) { return (int)lkpi_cpu_id(); }
#define raw_smp_processor_id() smp_processor_id()
#define num_online_cpus()      lkpi_cpu_count()
#define smp_mb()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_load_acquire(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* Run a function on every CPU. b1nix has no cross-CPU call IPI exposed to
 * drivers; this runs it on the calling CPU only. Every caller here uses it to
 * make a machine-wide state change take effect (cache flush, MSR write), so
 * missing the other CPUs is a real gap and is called out at each use. */
static inline void on_each_cpu(void (*func)(void *), void *info, int wait)
{ (void)wait; func(info); }

/* Pin to the current CPU across a short section. b1nix's preempt counter is
 * what pins; these are the paired increment and decrement. */
#define get_cpu()  ({ preempt_disable(); smp_processor_id(); })
#define put_cpu()  preempt_enable()

/*
 * Write back and invalidate every cache, on every CPU.
 *
 * Declared and deliberately not defined. WBINVD on the calling CPU alone is not
 * the operation — a driver calls this to make a coherency change visible
 * machine-wide before touching memory the GPU also reads, and doing it on one
 * CPU would leave stale lines on the others. b1nix has no cross-CPU call
 * mechanism for drivers, so a caller fails to link rather than getting a
 * partial flush that looks like a full one.
 */
void wbinvd_on_all_cpus(void);

#endif
