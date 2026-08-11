/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_RCULIST_H
#define LKPI_LINUX_RCULIST_H
#include <linux/list.h>
#include <lkpi/rcu.h>
/* List walks under an RCU read section. The publish/subscribe barriers are what
 * make a reader that sees a new node also see its contents. */
#define list_add_rcu(n, h)      list_add(n, h)
#define list_add_tail_rcu(n, h) list_add_tail(n, h)

/* Removing under RCU: the node is unlinked so no new reader can reach it, but
 * its own pointers are left alone — a reader already walking through it must
 * still be able to continue to the next node. That is the whole difference from
 * list_del, and clearing the pointers here would be the classic crash. */
static inline void list_del_rcu(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->prev = 0;
}


/*
 * Walking a list under RCU.
 *
 * The reader takes no lock; what it needs is that each link it follows was
 * published with a release, which rcu_dereference pairs with. b1nix's RCU read
 * sections disable interrupts (see <lkpi/rcu.h>), so a walk here cannot sleep
 * or migrate — the price is stated there and it applies to these loops.
 */
#define list_for_each_entry_rcu(pos, head, member, ...)                  \
	for (pos = list_entry(rcu_dereference((head)->next), __typeof__(*pos), member); \
	     &pos->member != (head);                                         \
	     pos = list_entry(rcu_dereference(pos->member.next), __typeof__(*pos), member))

#define list_entry_rcu(ptr, type, member) list_entry(rcu_dereference(ptr), type, member)
#define list_first_or_null_rcu(head, type, member) \
	({ struct list_head *__h = rcu_dereference((head)->next); \
	   __h != (head) ? list_entry(__h, type, member) : 0; })
#define hlist_for_each_entry_rcu(pos, head, member) \
	hlist_for_each_entry(pos, head, member)

#endif
