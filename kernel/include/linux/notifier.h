/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_NOTIFIER_H
#define LKPI_LINUX_NOTIFIER_H
#include <linux/kernel.h>
#include <linux/mutex.h>
/* A priority-ordered callback chain. The core uses one to tell interested
 * parties about hotplug and suspend; the list is walked until a callback
 * returns NOTIFY_STOP_MASK, which is what lets one consumer claim an event. */
#define NOTIFY_DONE      0x0000
#define NOTIFY_OK        0x0001
#define NOTIFY_STOP_MASK 0x8000
#define NOTIFY_BAD       (NOTIFY_STOP_MASK | 0x0002)
#define NOTIFY_STOP      (NOTIFY_OK | NOTIFY_STOP_MASK)

struct notifier_block;
typedef int (*notifier_fn_t)(struct notifier_block *nb, unsigned long action,
                             void *data);
struct notifier_block {
	notifier_fn_t notifier_call;
	struct notifier_block *next;
	int priority;
};
struct blocking_notifier_head { struct notifier_block *head; struct lkpi_mutex lock; };
struct atomic_notifier_head { struct notifier_block *head; };

int blocking_notifier_chain_register(struct blocking_notifier_head *nh,
                                     struct notifier_block *nb);
int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh,
                                       struct notifier_block *nb);
int blocking_notifier_call_chain(struct blocking_notifier_head *nh,
                                 unsigned long val, void *v);
#define BLOCKING_NOTIFIER_HEAD(name) struct blocking_notifier_head name
#define ATOMIC_NOTIFIER_HEAD(name) struct atomic_notifier_head name

/* Static initialiser for a blocking notifier chain. The lock and the head are
 * both established, because a chain that was only zeroed is not an empty list —
 * the same trap as the wait queue's head. */
#define ATOMIC_INIT_NOTIFIER_HEAD(name) do { BLOCKING_INIT_NOTIFIER_HEAD(name); } while (0)
#define BLOCKING_INIT_NOTIFIER_HEAD(name) do { (name)->head = 0; } while (0)


/* Fire an atomic notifier chain. b1nix has no notifier chains — nothing here
 * registers on one — so a chain is always empty and the call reports that no
 * callback stopped it. */
struct atomic_notifier_head;
static inline int atomic_notifier_call_chain(struct atomic_notifier_head *nh,
                                             unsigned long val, void *v)
{ (void)nh; (void)val; (void)v; return 0; }

#endif
