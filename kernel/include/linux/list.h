/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_LIST_H
#define LKPI_LINUX_LIST_H

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

#endif
