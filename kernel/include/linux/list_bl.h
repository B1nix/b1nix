/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_LIST_BL_H
#define LKPI_LINUX_LIST_BL_H

#include <linux/types.h>
#include <linux/bit_spinlock.h>

/*
 * A hash list whose head's low bit IS the lock.
 *
 * Upstream's trick, kept rather than replaced by a list plus a separate
 * spinlock, because the reason for it is structural: a buffer-head hash has one
 * of these per bucket, and a per-bucket spinlock would double the table. The
 * cost is that the head pointer must be masked before it is dereferenced, which
 * is what `hlist_bl_first` is for — and why nothing may dereference `first`
 * directly.
 */

#define LIST_BL_LOCKMASK 1UL

struct hlist_bl_head {
	struct hlist_bl_node *first;
};

struct hlist_bl_node {
	struct hlist_bl_node *next, **pprev;
};

#define INIT_HLIST_BL_HEAD(ptr) ((ptr)->first = NULL)

static inline void INIT_HLIST_BL_NODE(struct hlist_bl_node *h)
{
	h->next = NULL;
	h->pprev = NULL;
}

#define hlist_bl_entry(ptr, type, member) container_of(ptr, type, member)

static inline bool hlist_bl_unhashed(const struct hlist_bl_node *h)
{
	return !h->pprev;
}

static inline struct hlist_bl_node *hlist_bl_first(struct hlist_bl_head *h)
{
	return (struct hlist_bl_node *)((unsigned long)h->first & ~LIST_BL_LOCKMASK);
}

static inline void hlist_bl_set_first(struct hlist_bl_head *h,
                                      struct hlist_bl_node *n)
{
	/* The lock bit is preserved across the store; dropping it here would
	 * silently release a lock somebody else is holding. */
	h->first = (struct hlist_bl_node *)((unsigned long)n |
	                                    ((unsigned long)h->first & LIST_BL_LOCKMASK));
}

static inline bool hlist_bl_empty(const struct hlist_bl_head *h)
{
	return !((unsigned long)h->first & ~LIST_BL_LOCKMASK);
}

static inline void hlist_bl_add_head(struct hlist_bl_node *n,
                                     struct hlist_bl_head *h)
{
	struct hlist_bl_node *first = hlist_bl_first(h);

	n->next = first;
	if (first)
		first->pprev = &n->next;
	n->pprev = &h->first;
	hlist_bl_set_first(h, n);
}

static inline void __hlist_bl_del(struct hlist_bl_node *n)
{
	struct hlist_bl_node *next = n->next;
	struct hlist_bl_node **pprev = n->pprev;

	*pprev = (struct hlist_bl_node *)((unsigned long)next |
	                                  ((unsigned long)*pprev & LIST_BL_LOCKMASK));
	if (next)
		next->pprev = pprev;
}

static inline void hlist_bl_del(struct hlist_bl_node *n)
{
	__hlist_bl_del(n);
	n->next = NULL;
	n->pprev = NULL;
}

static inline void hlist_bl_del_init(struct hlist_bl_node *n)
{
	if (!hlist_bl_unhashed(n))
		hlist_bl_del(n);
}

static inline void hlist_bl_lock(struct hlist_bl_head *b)
{
	bit_spin_lock(0, (unsigned long *)b);
}

static inline void hlist_bl_unlock(struct hlist_bl_head *b)
{
	__bit_spin_unlock(0, (unsigned long *)b);
}

static inline bool hlist_bl_is_locked(struct hlist_bl_head *b)
{
	return ((unsigned long)b->first & LIST_BL_LOCKMASK) != 0;
}

#define hlist_bl_for_each_entry(tpos, pos, head, member)                       \
	for ((pos) = hlist_bl_first(head);                                         \
	     (pos) && ({ (tpos) = hlist_bl_entry(pos, __typeof__(*(tpos)), member); 1; }); \
	     (pos) = (pos)->next)

#define hlist_bl_for_each_entry_safe(tpos, pos, n, head, member)               \
	for ((pos) = hlist_bl_first(head);                                         \
	     (pos) && ({ (n) = (pos)->next; 1; }) &&                               \
	     ({ (tpos) = hlist_bl_entry(pos, __typeof__(*(tpos)), member); 1; });   \
	     (pos) = (n))

#endif
