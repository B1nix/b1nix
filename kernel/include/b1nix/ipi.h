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

#endif /* B1NIX_IPI_H */
