/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_RCUPDATE_H
#define LKPI_LINUX_RCUPDATE_H
#include <lkpi/rcu.h>

/* Initialising an rcu_head. Nothing tracks them here, so this only has to leave
 * the members in a defined state — a caller that later queues it will overwrite
 * both. */
#define init_rcu_head(head)      do { (head)->next = 0; (head)->func = 0; } while (0)
#define destroy_rcu_head(head)   do { (void)(head); } while (0)
#define init_rcu_head_on_stack(head)    init_rcu_head(head)
#define destroy_rcu_head_on_stack(head) destroy_rcu_head(head)


/* The expedited flavour forces a grace period rather than waiting for one to
 * come around. b1nix's RCU already ends a grace period as soon as every CPU has
 * been observed quiescent, so this is the same wait under a name that asks for
 * urgency there is no slower path to contrast with. */
#define synchronize_rcu_expedited() synchronize_rcu()

/* Sample the grace-period counter, and wait only if it has not advanced since.
 * b1nix's RCU exposes no counter, so the sample is a constant and the
 * conditional wait is an unconditional one — correct, and slower than upstream
 * in the case where no wait was needed. */
static inline unsigned long get_state_synchronize_rcu(void) { return 0; }
static inline void cond_synchronize_rcu(unsigned long oldstate)
{ (void)oldstate; synchronize_rcu(); }

#endif
