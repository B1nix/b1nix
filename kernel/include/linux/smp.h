/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SMP_H
#define LKPI_LINUX_SMP_H

/* The barriers are architecture's to define and <asm/barrier.h> defines them;
 * this header used to carry its own smp_mb and left smp_rmb/smp_wmb undefined,
 * so code that used all three resolved two of them and implicitly declared the
 * third. */
#include <asm/barrier.h>
#include <lkpi/env.h>
#include <linux/types.h>
/* nr_cpu_ids and the for_each_*_cpu loops. Imported code uses them from files
 * that include only <linux/smp.h>. */
#include <linux/cpumask.h>
static inline int smp_processor_id(void) { return (int)lkpi_cpu_id(); }
#define raw_smp_processor_id() smp_processor_id()
#define num_online_cpus()      lkpi_cpu_count()
/* smp_mb/smp_rmb/smp_wmb and the acquire/release accessors come from
 * <asm/barrier.h> above. They were defined here as well, which was a
 * redefinition of the same names with the same meaning — harmless until one of
 * the two changed. */

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
int wbinvd_on_all_cpus(void);

#endif
