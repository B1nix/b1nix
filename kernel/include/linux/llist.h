/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_LLIST_H
#define LKPI_LINUX_LLIST_H

#include <linux/kernel.h>
#include <linux/types.h>

/*
 * Lock-free singly-linked stack.
 *
 * Producers push with one compare-exchange and never block, which is why this
 * is what a driver uses to hand work out of an interrupt handler. The consumer
 * takes the whole list in one exchange rather than popping, so there is exactly
 * one writer of the head at a time on the consuming side.
 *
 * Order is reversed by construction: pushes go on the front, so a drained batch
 * is newest-first. Callers that need submission order reverse it themselves —
 * llist_reverse_order exists for exactly that, and pretending the list were
 * FIFO would be the quiet kind of wrong.
 */

struct llist_node {
	struct llist_node *next;
};

struct llist_head {
	struct llist_node *first;
};

#define LLIST_HEAD_INIT(name) { 0 }
#define LLIST_HEAD(name) struct llist_head name = LLIST_HEAD_INIT(name)

static inline void init_llist_head(struct llist_head *head)
{
	head->first = 0;
}

static inline bool llist_empty(const struct llist_head *head)
{
	return __atomic_load_n(&head->first, __ATOMIC_ACQUIRE) == 0;
}

/* Returns true if the list was empty before this push — the signal a caller
 * uses to decide whether it also needs to kick the consumer. */
static inline bool llist_add(struct llist_node *new_node,
                             struct llist_head *head)
{
	struct llist_node *first = __atomic_load_n(&head->first, __ATOMIC_ACQUIRE);
	do {
		new_node->next = first;
	} while (!__atomic_compare_exchange_n(&head->first, &first, new_node, 1,
	                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	return first == 0;
}

/* Take the whole list. */
static inline struct llist_node *llist_del_all(struct llist_head *head)
{
	return __atomic_exchange_n(&head->first, (struct llist_node *)0,
	                           __ATOMIC_ACQ_REL);
}

static inline struct llist_node *llist_del_first(struct llist_head *head)
{
	struct llist_node *first = __atomic_load_n(&head->first, __ATOMIC_ACQUIRE);
	while (first) {
		if (__atomic_compare_exchange_n(&head->first, &first, first->next, 1,
		                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return first;
	}
	return 0;
}

static inline struct llist_node *llist_reverse_order(struct llist_node *head)
{
	struct llist_node *prev = 0;
	while (head) {
		struct llist_node *next = head->next;
		head->next = prev;
		prev = head;
		head = next;
	}
	return prev;
}

#define llist_entry(ptr, type, member) container_of(ptr, type, member)

#define llist_for_each(pos, node) \
	for ((pos) = (node); pos; (pos) = (pos)->next)

#define llist_for_each_safe(pos, n, node)                     \
	for ((pos) = (node); (pos) && ((n) = (pos)->next, true);  \
	     (pos) = (n))

#define llist_for_each_entry(pos, node, member)                            \
	for ((pos) = (node) ? llist_entry(node, __typeof__(*(pos)), member) : 0; \
	     (pos);                                                            \
	     (pos) = (pos)->member.next                                        \
	                 ? llist_entry((pos)->member.next, __typeof__(*(pos)), \
	                               member)                                 \
	                 : 0)

#define llist_for_each_entry_safe(pos, n, node, member)                    \
	for ((pos) = (node) ? llist_entry(node, __typeof__(*(pos)), member) : 0; \
	     (pos) && ((n) = (pos)->member.next                                \
	                        ? llist_entry((pos)->member.next,              \
	                                      __typeof__(*(pos)), member)      \
	                        : 0,                                           \
	               true);                                                  \
	     (pos) = (n))


/* Push a whole already-linked batch onto the head at once. The batch's last
 * node is given, so the splice is a single exchange rather than one per node. */
static inline bool llist_add_batch(struct llist_node *new_first,
                                   struct llist_node *new_last,
                                   struct llist_head *head)
{
	struct llist_node *first;
	do {
		first = head->first;
		new_last->next = first;
	} while (!__atomic_compare_exchange_n(&head->first, &first, new_first,
	                                      false, __ATOMIC_SEQ_CST,
	                                      __ATOMIC_SEQ_CST));
	return first == NULL;
}

#endif
