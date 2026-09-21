/* SPDX-License-Identifier: GPL-2.0-only */
/* NUMA memory policy: mbind(2), set_mempolicy(2), get_mempolicy(2) and
 * set_mempolicy_home_node(2), and the answer the page-fault path needs from
 * them — which node this allocation should come from (M124 for the calls,
 * M128 for the behaviour behind them). */
#ifndef B1NIX_MEMPOLICY_H
#define B1NIX_MEMPOLICY_H

#include <b1nix/types.h>

struct vm_area;

/* The system calls. Each returns 0 or a negative errno. */
isize mempolicy_mbind(u64 start, u64 len, u64 mode, u64 nmask, u64 maxnode,
                      u64 flags);
isize mempolicy_set(u64 mode, u64 nmask, u64 maxnode);
isize mempolicy_get(u64 umode, u64 nmask, u64 maxnode, u64 addr, u64 flags);
isize mempolicy_home_node(u64 start, u64 len, u64 node, u64 flags);

/* Per-task state, kept beside the task like the rest of linux_modern's. */
void mempolicy_task_reset(usize slot);
void mempolicy_fork_inherit(usize parent_slot, usize child_slot);

/* Which node a page for `vaddr` in `vma` should come from, for the task
 * running now: the mapping's own policy first, then the task's, then the node
 * this CPU sits on. `strict` is set when the policy is a binding the caller
 * asked to be honoured exactly (MPOL_BIND), so an allocation elsewhere is a
 * failure rather than a fallback. Returns the node, or -1 for "anywhere". */
int mempolicy_node_for(struct vm_area *vma, u64 vaddr, int *strict);

#endif
