/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_HASHTABLE_H
#define LKPI_LINUX_HASHTABLE_H
#include <linux/hash.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/types.h>

/*
 * A fixed-size chained hash table, declared inline in the user's structure.
 *
 * Written out rather than forwarded: the whole interface is macros over an
 * array of hlist heads, so there is nothing underneath to forward to. The bucket
 * count is a power of two and the index is taken from the top bits of a
 * multiplicative hash, which is what makes hash_min's shift correct.
 */
#define DEFINE_HASHTABLE(name, bits) \
	struct hlist_head name[1 << (bits)] = { [0 ... ((1 << (bits)) - 1)] = HLIST_HEAD_INIT }
#define DECLARE_HASHTABLE(name, bits) struct hlist_head name[1 << (bits)]
#define HASH_SIZE(name) (ARRAY_SIZE(name))
#define HASH_BITS(name) ilog2(HASH_SIZE(name))

#define hash_min(val, bits) \
	(sizeof(val) <= 4 ? hash_32((u32)(val), bits) : hash_long((unsigned long)(val), bits))

#define hash_init(hashtable)                                        \
	do {                                                            \
		unsigned int __i;                                           \
		for (__i = 0; __i < HASH_SIZE(hashtable); __i++)            \
			INIT_HLIST_HEAD(&(hashtable)[__i]);                     \
	} while (0)

#define hash_add(hashtable, node, key) \
	hlist_add_head(node, &(hashtable)[hash_min(key, HASH_BITS(hashtable))])

#define hash_del(node) hlist_del_init(node)
#define hash_hashed(node) (!hlist_unhashed(node))

#define hash_for_each_possible(name, obj, member, key) \
	hlist_for_each_entry(obj, &name[hash_min(key, HASH_BITS(name))], member)

#define hash_for_each_possible_safe(name, obj, tmp, member, key)      \
	hlist_for_each_entry_safe(obj, tmp, &name[hash_min(key, HASH_BITS(name))], member)

#define hash_for_each(name, bkt, obj, member)                         \
	for ((bkt) = 0; (bkt) < HASH_SIZE(name); (bkt)++)                 \
		hlist_for_each_entry(obj, &name[bkt], member)

#define hash_for_each_safe(name, bkt, tmp, obj, member)               \
	for ((bkt) = 0; (bkt) < HASH_SIZE(name); (bkt)++)                 \
		hlist_for_each_entry_safe(obj, tmp, &name[bkt], member)

#define hash_empty(hashtable)                                         \
	({                                                                \
		unsigned int __i; bool __e = true;                            \
		for (__i = 0; __i < HASH_SIZE(hashtable); __i++)              \
			if (!hlist_empty(&(hashtable)[__i])) { __e = false; break; } \
		__e;                                                          \
	})
#endif
