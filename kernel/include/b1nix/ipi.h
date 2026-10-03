/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_IPI_H
#define B1NIX_IPI_H

#include <b1nix/types.h>

/* Cross-CPU IPI primitives that aren't TLB shootdown (which has its own
 * header in b1nix/tlb.h). Today the only one is the reschedule IPI: wake
 * every other CPU out of `sti; hlt` so it re-polls the global runqueue
 * after a new task has been published. Safe to call unconditionally — a
 * single-CPU build short-circuits to a no-op. */

/* Wake every other online CPU. Sends RESCHEDULE_VECTOR to all-but-self. */
void ipi_reschedule_all(void);

/* Run fn(info) on `cpu`, in its interrupt context with interrupts off; on
 * this CPU it simply runs. With `wait`, return only once it has run. 0, or
 * -EINVAL / -ENXIO for a CPU that is not there. x86_64 only. */
int smp_call_on_cpu(int cpu, void (*fn)(void *), void *info, int wait);
void smp_call_handler(void);
/* Run a request aimed at this CPU, if one is waiting (spin-wait loops). */
void smp_call_poll_pending(void);

#endif /* B1NIX_IPI_H */
