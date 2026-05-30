#ifndef B1NIX_BKL_H
#define B1NIX_BKL_H

#include <b1nix/types.h>

/* ── Big Kernel Lock ──────────────────────────────────────────────────────
 *
 * A single recursive lock that serialises kernel-mode execution across all
 * CPUs. A CPU holds the BKL whenever it runs kernel code on behalf of a task;
 * it releases the lock when entering userspace (ring 3) or when going idle.
 * This lets several CPUs run userspace processes in parallel while only one
 * core is ever inside the (largely un-fine-grained) kernel at a time.
 *
 * The lock is keyed on cpu_id, so the owning CPU may re-enter recursively —
 * e.g. an interrupt that fires while the same core is mid-syscall just bumps
 * the depth instead of self-deadlocking.
 *
 * Locking discipline (see docs/roadmap.md M24b and kernel/sched/bkl.c):
 *   - Userspace->kernel entry acquires (syscall_entry.S, the IRQ/exception
 *     handlers); kernel->userspace return releases (syscall_entry.S,
 *     user_jump.S).
 *   - Every arch_context_switch happens with the current CPU holding the BKL
 *     at depth exactly 1, and the lock is handed off across the switch (same
 *     CPU, owner unchanged) — so the resumed task inherits depth 1.
 *   - An idle CPU drops the BKL (bkl_unlock) before parking and re-takes it
 *     (bkl_lock) before scheduling again.
 */

void bkl_lock(void);
void bkl_unlock(void);
int bkl_is_held_by_current_cpu(void);
void bkl_lock_for_switch(u32 depth);
void bkl_unlock_for_switch(void);
u32 bkl_get_depth(void);

#endif /* B1NIX_BKL_H */
