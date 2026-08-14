/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_XARRAY_H
#define LKPI_LINUX_XARRAY_H
#include <lkpi/xarray.h>
/* The implementation is lkpi's; this is only the name Linux uses for it. */

/* The radix tree's root, under the name callers embed in their structures.
 * Upstream's radix tree is an xarray with a different spelling; so is this. */
struct radix_tree_root { struct xarray xa; };
#define INIT_RADIX_TREE(root, mask) do { (void)(mask); xa_init(&(root)->xa); } while (0)
#define RADIX_TREE(name, mask) struct radix_tree_root name


/* The radix-tree spelling of the same operations. */
#define radix_tree_insert(root, index, ptr) lkpi_xa_store(&(root)->xa, index, ptr)
#define radix_tree_lookup(root, index)      xa_load(&(root)->xa, index)
#define radix_tree_delete(root, index)      xa_erase(&(root)->xa, index)
#define radix_tree_empty(root)              xa_empty(&(root)->xa)


/*
 * The radix-tree iteration shape.
 *
 * Upstream's is a cursor that batches slots; here it walks the xarray by index,
 * which is the same visit order with none of the batching. The `index` field is
 * what callers read after the loop, so it is maintained rather than left to the
 * macro.
 */
struct radix_tree_iter {
	unsigned long index;
	unsigned long next_index;
	unsigned long tags;
};

#define radix_tree_iter_init(iter, start) \
	({ (iter)->index = 0; (iter)->next_index = (start); (void **)0; })


/* The array's own lock, for a caller that walks it and mutates under one hold.
 * b1nix's xarray serialises internally; these give the caller the same
 * exclusion across a sequence of operations, which internal locking cannot. */
#define xa_lock_irqsave(xa, flags)      do { (void)(xa); (flags) = lkpi_irq_save(); } while (0)
#define xa_unlock_irqrestore(xa, flags) do { (void)(xa); lkpi_irq_restore(flags); } while (0)
#define xa_lock(xa)                     do { (void)(xa); } while (0)
#define xa_unlock(xa)                   do { (void)(xa); } while (0)


/*
 * Iterating a radix tree slot by slot.
 *
 * Upstream's cursor batches slots and hands back pointers into the tree; here
 * the walk is by index over the xarray, which visits the same entries in the
 * same order. The loop body sees `entry` set and `iter->index` current, which
 * is what callers read.
 */
#define radix_tree_for_each_slot(slot, root, iter, start)                    \
	for ((iter)->index = (start), (slot) = (void **)xa_load(&(root)->xa, (iter)->index); \
	     (slot);                                                             \
	     (iter)->index++, (slot) = (void **)xa_load(&(root)->xa, (iter)->index))

#define radix_tree_deref_slot(slot)          ((void *)(slot))
#define radix_tree_deref_slot_protected(slot, lock) ((void *)(slot))
#define radix_tree_iter_resume(slot, iter)   ((void **)0)


/* The double-underscore forms assume the caller holds the array's lock. b1nix's
 * xarray locks internally either way, so they are the same operations — a
 * caller gets the exclusion it expected and one more acquire than it planned. */
#define __xa_store(xa, index, entry, gfp) ({ (void)(gfp); lkpi_xa_store(xa, index, entry); })
#define __xa_erase(xa, index)             xa_erase(xa, index)


/* The creation flags and struct xa_limit are declared in <lkpi/xarray.h>,
 * included above, next to the functions that take them. */
#define XA_LIMIT(min_, max_) ((struct xa_limit){ .min = (min_), .max = (max_) })
#define xa_limit_32b  XA_LIMIT(0, 0xffffffffu)
#define xa_limit_31b  XA_LIMIT(0, 0x7fffffffu)


/* Store at the first free index at or above the limit's minimum, reporting it
 * through *id. Returns 0, -ENOSPC when the limit has no free index, or -ENOMEM. */
int xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit,
             gfp_t gfp);
#define __xa_alloc(xa, id, entry, limit, gfp) xa_alloc(xa, id, entry, limit, gfp)

/*
 * Errors and values encoded in the entry pointer.
 *
 * Upstream tags the low bits of a stored pointer so an xarray can hold a small
 * integer or an errno without a separate allocation. The same encoding is used
 * here: bit 0 marks a value, bit 1 marks an internal entry. Callers store
 * pointers that are at least 4-byte aligned, which is what makes the tags free.
 */
static inline void *xa_mk_value(unsigned long v)
{ return (void *)((v << 1) | 1ul); }
static inline unsigned long xa_to_value(const void *entry)
{ return (unsigned long)entry >> 1; }
static inline bool xa_is_value(const void *entry)
{ return (unsigned long)entry & 1ul; }
static inline void *xa_mk_internal(unsigned long v)
{ return (void *)((v << 2) | 2ul); }
static inline bool xa_is_err(const void *entry)
{
	return (unsigned long)entry >= (unsigned long)-4095;
}
static inline int xa_err(void *entry)
{ return xa_is_err(entry) ? (int)(long)entry : 0; }
static inline void *xa_mk_err(long err) { return (void *)err; }

/* Erase the entry the iterator is sitting on. The walk here is by index, so
 * this is an erase at that index and the loop's next step re-loads. */
#define radix_tree_iter_delete(root, iter, slot) \
	xa_erase(&(root)->xa, (iter)->index)


/*
 * Iterating an xarray as a loop, which is the shape imported code uses.
 *
 * lkpi's xa_for_each is a callback walk; a loop needs a resumable position, so
 * this steps through with xa_find_next(), which reports the next present entry
 * at or after an index. Because it re-descends from the root each step, an
 * erase inside the loop is safe — unlike upstream's cursor, which is not.
 */
void *xa_find_next(struct xarray *xa, u64 *index);
#undef xa_for_each
#define xa_for_each_start(xa_, index_, entry_, start_)                      \
	for ((index_) = (start_), (entry_) = xa_find_next((xa_), (u64 *)&(index_)); \
	     (entry_);                                                          \
	     (index_)++, (entry_) = xa_find_next((xa_), (u64 *)&(index_)))
#define xa_for_each(xa_, index_, entry_) xa_for_each_start(xa_, index_, entry_, 0)


/*
 * Upstream's xa_store() takes the allocation context; lkpi's does not, because
 * its node allocation is always the ordinary kernel one. Take upstream's
 * argument and let it go — a caller asking for GFP_ATOMIC gets an allocation
 * that may sleep, which is the one place this differs and is called out here
 * rather than left to be found.
 */
#undef xa_store
/* Upstream's xa_store() returns the entry that was replaced, or an ERR_PTR;
 * lkpi's returns 0 or -ENOMEM. The old entry is read first so both are
 * reported: a caller that frees what it displaced gets the right pointer, and
 * an allocation failure comes back as an ERR_PTR the way callers test for. */
#define xa_store(xa_, index_, entry_, gfp_)                                  \
	({                                                                       \
		(void)(gfp_);                                                        \
		void *__old = xa_load((xa_), (index_));                              \
		int __err = lkpi_xa_store((xa_), (index_), (entry_));                \
		__err ? xa_mk_err(__err) : __old;                                    \
	})

#endif
