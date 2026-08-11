/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_LIST_H
#define LKPI_LINUX_LIST_H
/* Upstream's list.h carries the poison values a removed node is left with;
 * callers reach them through it rather than including poison.h themselves. */
#include <linux/poison.h>

#include <b1nix/types.h>
#include <linux/container_of.h>

/* Defined here rather than in <linux/types.h>, which includes this header:
 * whichever of the two owns the definition, the other must only forward it. */
struct list_head {
	struct list_head *next, *prev;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

struct hlist_head {
	struct hlist_node *first;
};

/*
 * Intrusive circular doubly-linked lists.
 *
 * The head is a node like any other, which is what removes every special case:
 * an empty list is a head pointing at itself, so insertion and removal never
 * have to ask whether they are at an end. Deleted nodes are poisoned rather
 * than left pointing into the list, so a use-after-delete faults on a
 * recognisable address instead of quietly walking freed memory.
 */

#define LIST_POISON1 ((void *)0x100)
#define LIST_POISON2 ((void *)0x200)

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *item, struct list_head *prev,
                              struct list_head *next)
{
	next->prev = item;
	item->next = next;
	item->prev = prev;
	prev->next = item;
}

static inline void list_add(struct list_head *item, struct list_head *head)
{
	__list_add(item, head, head->next);
}

static inline void list_add_tail(struct list_head *item, struct list_head *head)
{
	__list_add(item, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

/* Unlink without poisoning, for a caller that re-uses the entry immediately. */
static inline void __list_del_entry(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->next = (struct list_head *)LIST_POISON1;
	entry->prev = (struct list_head *)LIST_POISON2;
}

static inline void list_del_init(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	INIT_LIST_HEAD(entry);
}

static inline void list_move(struct list_head *item, struct list_head *head)
{
	__list_del(item->prev, item->next);
	list_add(item, head);
}

static inline void list_move_tail(struct list_head *item,
                                  struct list_head *head)
{
	__list_del(item->prev, item->next);
	list_add_tail(item, head);
}

static inline int list_empty(const struct list_head *head)
{
	return head->next == head;
}

static inline int list_is_last(const struct list_head *item,
                               const struct list_head *head)
{
	return item->next == head;
}

static inline int list_is_first(const struct list_head *item,
                                const struct list_head *head)
{
	return item->prev == head;
}

static inline int list_is_singular(const struct list_head *head)
{
	return !list_empty(head) && head->next == head->prev;
}

static inline void list_replace(struct list_head *old, struct list_head *item)
{
	item->next = old->next;
	item->next->prev = item;
	item->prev = old->prev;
	item->prev->next = item;
}

static inline void list_splice_tail(struct list_head *list,
                                    struct list_head *head)
{
	if (list_empty(list))
		return;
	struct list_head *first = list->next;
	struct list_head *last = list->prev;
	struct list_head *at = head->prev;
	first->prev = at;
	at->next = first;
	last->next = head;
	head->prev = last;
	INIT_LIST_HEAD(list);
}

static inline void list_splice(struct list_head *list, struct list_head *head)
{
	if (list_empty(list))
		return;
	struct list_head *first = list->next;
	struct list_head *last = list->prev;
	first->prev = head;
	last->next = head->next;
	head->next->prev = last;
	head->next = first;
}

static inline void list_splice_init(struct list_head *list,
                                    struct list_head *head)
{
	if (list_empty(list))
		return;
	struct list_head *first = list->next;
	struct list_head *last = list->prev;
	first->prev = head;
	last->next = head->next;
	head->next->prev = last;
	head->next = first;
	INIT_LIST_HEAD(list);
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member) \
	list_entry((ptr)->prev, type, member)
#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_prev_entry(pos, member) \
	list_entry((pos)->member.prev, __typeof__(*(pos)), member)
#define list_first_entry_or_null(ptr, type, member) \
	(list_empty(ptr) ? 0 : list_first_entry(ptr, type, member))

#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head)                     \
	for (pos = (head)->next, n = pos->next; pos != (head);   \
	     pos = n, n = pos->next)

#define list_for_each_entry(pos, head, member)                            \
	for (pos = list_first_entry(head, __typeof__(*pos), member);          \
	     &pos->member != (head); pos = list_next_entry(pos, member))

#define list_for_each_entry_safe(pos, n, head, member)                    \
	for (pos = list_first_entry(head, __typeof__(*pos), member),          \
	    n = list_next_entry(pos, member);                                 \
	     &pos->member != (head); pos = n, n = list_next_entry(n, member))

#define list_for_each_entry_reverse(pos, head, member)                    \
	for (pos = list_last_entry(head, __typeof__(*pos), member);           \
	     &pos->member != (head); pos = list_prev_entry(pos, member))

#define list_for_each_entry_from(pos, head, member)                       \
	for (; &pos->member != (head); pos = list_next_entry(pos, member))

#define list_for_each_entry_from_reverse(pos, head, member)               \
	for (; &pos->member != (head); pos = list_prev_entry(pos, member))

#define list_for_each_entry_continue(pos, head, member)                   \
	for (pos = list_next_entry(pos, member); &pos->member != (head);      \
	     pos = list_next_entry(pos, member))

/* hlist: a head of one pointer, for hash buckets where halving the table's
 * size matters more than being able to walk backwards. */
#define HLIST_HEAD_INIT { .first = 0 }
static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
	h->next = 0;
	h->pprev = 0;
}
static inline int hlist_empty(const struct hlist_head *h) { return !h->first; }
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	n->next = h->first;
	if (h->first)
		h->first->pprev = &n->next;
	h->first = n;
	n->pprev = &h->first;
}
static inline void hlist_del_init(struct hlist_node *n)
{
	if (n->pprev) {
		*n->pprev = n->next;
		if (n->next)
			n->next->pprev = n->pprev;
		INIT_HLIST_NODE(n);
	}
}
#define hlist_entry(ptr, type, member) container_of(ptr, type, member)
#define hlist_for_each_entry(pos, head, member)                           \
	for (pos = (head)->first                                              \
	               ? hlist_entry((head)->first, __typeof__(*pos), member) \
	               : 0;                                                   \
	     pos;                                                             \
	     pos = pos->member.next                                           \
	               ? hlist_entry(pos->member.next, __typeof__(*pos), member) \
	               : 0)


/* Move a whole list to a new head, leaving the old one empty. list_replace is
 * already defined above; this is the pairing that also resets the old head, and
 * the empty case has to be handled separately — list_replace on an empty list
 * would leave the new head pointing at the old one. */
static inline void list_replace_init(struct list_head *old, struct list_head *new_)
{
	if (list_empty(old))
		INIT_LIST_HEAD(new_);
	else
		list_replace(old, new_);
	INIT_LIST_HEAD(old);
}


/* Re-anchor a safe iteration after the cursor's successor was removed inside
 * the loop. Without it the loop would resume from a node that is no longer on
 * the list. */
#define list_safe_reset_next(pos, n, member) \
	(n = list_next_entry(pos, member))

/* The RCU-safe operations live with the rest of the list interface: callers
 * reach them through either header, and upstream's rculist.h is an extension of
 * this one rather than a separate structure. */
#include <linux/rculist.h>


/* A walk with no locking and no RCU annotations: the caller has established by
 * other means that the list cannot change. Written separately from
 * list_for_each_entry so that claim is visible at the call site rather than
 * assumed. */
#define list_for_each_entry_lockless(pos, head, member) \
	list_for_each_entry(pos, head, member)


/* Safe iteration from the tail. Teardown wants it: freeing from the end means
 * a node's dependants are gone before it is. */
#define list_for_each_entry_safe_reverse(pos, n, head, member)             \
	for (pos = list_last_entry(head, __typeof__(*pos), member),             \
	     n = list_prev_entry(pos, member);                                  \
	     &pos->member != (head);                                            \
	     pos = n, n = list_prev_entry(n, member))


/* Walk backwards. Same loop as list_for_each with the links reversed. */
#define list_for_each_prev(pos, head) \
	for ((pos) = (head)->prev; (pos) != (head); (pos) = (pos)->prev)
#define list_for_each_prev_safe(pos, n, head) \
	for ((pos) = (head)->prev, (n) = (pos)->prev; (pos) != (head); \
	     (pos) = (n), (n) = (pos)->prev)

/* Emptiness test for a list being modified concurrently without the caller
 * holding its lock: upstream reads both links so a half-finished deletion is
 * not read as "empty". The same two reads here. */
static inline int list_empty_careful(const struct list_head *head)
{
	struct list_head *next = head->next;
	return (next == head) && (next == head->prev);
}

static inline void list_splice_tail_init(struct list_head *list,
                                         struct list_head *head)
{
	if (!list_empty(list)) {
		list_splice_tail(list, head);
		INIT_LIST_HEAD(list);
	}
}

/* Length of a list. O(n) — the callers are diagnostics. */
static inline usize list_count_nodes(struct list_head *head)
{
	struct list_head *pos;
	usize n = 0;
	list_for_each(pos, head)
		n++;
	return n;
}


#ifndef INIT_HLIST_HEAD
#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)
#endif


#define hlist_for_each_entry_safe(pos, n, head, member)                     \
	for ((pos) = (head)->first ?                                            \
	         hlist_entry((head)->first, typeof(*(pos)), member) : NULL;     \
	     (pos) && ((n) = (pos)->member.next, 1);                            \
	     (pos) = (n) ? hlist_entry((n), typeof(*(pos)), member) : NULL)


/* Continue a walk backwards from the current position. */
#define list_for_each_entry_continue_reverse(pos, head, member)             \
	for ((pos) = list_prev_entry(pos, member);                              \
	     &(pos)->member != (head);                                          \
	     (pos) = list_prev_entry(pos, member))

/* Move a run of entries — first through last, inclusive — to just after `head`.
 * One splice rather than a move per entry, which is the point. */
static inline void list_bulk_move_tail(struct list_head *head,
                                       struct list_head *first,
                                       struct list_head *last)
{
	first->prev->next = last->next;
	last->next->prev = first->prev;
	head->prev->next = first;
	first->prev = head->prev;
	last->next = head;
	head->prev = last;
}

#endif
