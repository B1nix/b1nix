/* SPDX-License-Identifier: GPL-2.0-only */
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

/*
 * Dereference under RCU with the caller asserting it holds something that makes
 * that safe — a lock, rather than rcu_read_lock. The condition is checked by
 * lockdep upstream and by nothing here, so the two spellings load the same way;
 * the name still records which promise the caller is making.
 */
#define rcu_dereference_check(p, c) ({ (void)(c); rcu_dereference(p); })
#define rcu_dereference_protected(p, c) ({ (void)(c); (p); })
#define rcu_dereference_raw(p) (p)
extern struct lockdep_map rcu_callback_map;

#endif
