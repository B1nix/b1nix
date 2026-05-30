#ifndef B1NIX_TLB_H
#define B1NIX_TLB_H

#include <b1nix/types.h>

/* TLB shootdown (M28 #5).
 *
 * On x86_64 each CPU caches address translations in its own TLB. When one CPU
 * unmaps a page from a shared address space (kernel space, or a multi-threaded
 * process), the OTHER CPUs' TLBs still hold stale entries — a write to the
 * supposedly-unmapped page goes to the freed frame. We close that by sending
 * an IPI (TLB_SHOOTDOWN_VECTOR) to the other CPUs; the handler executes
 * `invlpg` for the requested vaddr and decrements a pending counter. The
 * initiator polls the counter to zero before returning, so vmm_unmap_page is
 * fully synchronous from the caller's point of view.
 *
 * Single-CPU builds and pre-SMP boot skip the IPI entirely — no-op.
 */

/* Invalidate a single page on every other online CPU. The local TLB is the
 * caller's responsibility (vmm_unmap_page already does `invlpg` locally).
 * Synchronous: returns after every target ACKs (or after a generous timeout
 * fires, in which case we panic since stale TLBs would corrupt user memory). */
void tlb_shootdown_page(u64 vaddr);

/* Invalidate every page on every other online CPU by writing CR3. Used by
 * paths that bulk-change a large region (e.g. paging_free_address_space).
 * Same synchronous contract. */
void tlb_shootdown_all(void);

/* IPI handler entry point — called from x86_irq_handler_inner for
 * TLB_SHOOTDOWN_VECTOR. Must EOI itself. */
void tlb_shootdown_handler(void);

/* Runtime gate on whether tlb_shootdown_page / _all actually fire an IPI.
 * Default OFF: under the current Big Kernel Lock model (M24b), only one CPU
 * is in kernel code at a time, so cross-CPU TLB races on kernel-half pages
 * cannot occur — issuing IPIs into APs that may be running with IRQs
 * disabled (scheduler_waitpid, page-fault path) is a deadlock and adds
 * latency to every unmap. M28 #7 (BKL removal) will call this with `1` as
 * the final step of dismantling the BKL, after which the infrastructure
 * built in M28 #5 is load-bearing. */
void tlb_shootdown_set_enabled(int enabled);

#endif /* B1NIX_TLB_H */
