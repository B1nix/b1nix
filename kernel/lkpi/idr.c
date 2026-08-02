/*
 * SPDX-License-Identifier: MIT
 *
 * M99 linuxkpi: id-to-pointer map. See kernel/include/lkpi/idr.h.
 */

#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <lkpi/idr.h>
#include <string.h>

#define IDR_INITIAL_SLOTS 16
/* Ceiling on a single idr. GEM handle tables and object id spaces stay far
 * below this; the bound turns a runaway allocator into -ENOSPC rather than a
 * kernel that eats all of memory one doubling at a time. */
#define IDR_MAX_SLOTS (1u << 20)

void idr_init_base(struct idr *idr, u32 base)
{
	if (!idr)
		return;
	idr->slots = 0;
	idr->capacity = 0;
	idr->hint = 0;
	idr->count = 0;
	idr->base = base;
	idr->lock = SPINLOCK_INIT;
}

void idr_destroy(struct idr *idr)
{
	if (!idr)
		return;
	u64 flags;
	spin_lock_irqsave(&idr->lock, &flags);
	void **old = idr->slots;
	idr->slots = 0;
	idr->capacity = 0;
	idr->count = 0;
	idr->hint = 0;
	spin_unlock_irqrestore(&idr->lock, flags);
	if (old)
		kfree(old);
}

/* Grow to at least `need` slots. Caller holds the lock; the allocation happens
 * with interrupts disabled, which kmalloc supports (it never blocks). */
static int idr_grow_locked(struct idr *idr, u32 need)
{
	if (need <= idr->capacity)
		return 0;
	if (need > IDR_MAX_SLOTS)
		return -ENOSPC;

	u32 cap = idr->capacity ? idr->capacity : IDR_INITIAL_SLOTS;
	while (cap < need) {
		if (cap > IDR_MAX_SLOTS / 2) {
			cap = IDR_MAX_SLOTS;
			break;
		}
		cap *= 2;
	}
	if (cap < need)
		return -ENOSPC;

	void **slots = kzalloc((usize)cap * sizeof(void *));
	if (!slots)
		return -ENOMEM;
	if (idr->slots) {
		memcpy(slots, idr->slots, (usize)idr->capacity * sizeof(void *));
		kfree(idr->slots);
	}
	idr->slots = slots;
	idr->capacity = cap;
	return 0;
}

/* Translate an external id into a slot index, or -1 when it is below base. */
static long idr_slot_of(const struct idr *idr, u32 id)
{
	if (id < idr->base)
		return -1;
	return (long)(id - idr->base);
}

int idr_alloc(struct idr *idr, void *ptr, u32 start, u32 end)
{
	if (!idr || !ptr)
		return -EINVAL;

	u32 lo = start > idr->base ? start : idr->base;
	if (end && end <= lo)
		return -ENOSPC;

	u64 flags;
	spin_lock_irqsave(&idr->lock, &flags);

	u32 lo_slot = lo - idr->base;
	u32 hi_slot = end ? (end - idr->base) : IDR_MAX_SLOTS;

	/* Start from the rotating hint when it is inside the requested range —
	 * consecutive allocations then cost one probe instead of a scan over every
	 * live handle. */
	u32 first = (idr->hint > lo_slot && idr->hint < hi_slot) ? idr->hint : lo_slot;

	for (int pass = 0; pass < 2; pass++) {
		u32 from = pass == 0 ? first : lo_slot;
		u32 to = pass == 0 ? hi_slot : first;
		for (u32 i = from; i < to; i++) {
			if (i >= idr->capacity) {
				if (idr_grow_locked(idr, i + 1) < 0) {
					spin_unlock_irqrestore(&idr->lock, flags);
					return i + 1 > IDR_MAX_SLOTS ? -ENOSPC : -ENOMEM;
				}
			}
			if (idr->slots[i])
				continue;
			idr->slots[i] = ptr;
			idr->count++;
			idr->hint = i + 1;
			u32 id = idr->base + i;
			spin_unlock_irqrestore(&idr->lock, flags);
			return (int)id;
		}
	}

	spin_unlock_irqrestore(&idr->lock, flags);
	return -ENOSPC;
}

int idr_alloc_at(struct idr *idr, void *ptr, u32 id)
{
	if (!idr || !ptr)
		return -EINVAL;
	long slot = idr_slot_of(idr, id);
	if (slot < 0 || (u32)slot >= IDR_MAX_SLOTS)
		return -EINVAL;

	u64 flags;
	spin_lock_irqsave(&idr->lock, &flags);
	int rc = idr_grow_locked(idr, (u32)slot + 1);
	if (rc < 0) {
		spin_unlock_irqrestore(&idr->lock, flags);
		return rc;
	}
	if (idr->slots[slot]) {
		spin_unlock_irqrestore(&idr->lock, flags);
		return -EBUSY;
	}
	idr->slots[slot] = ptr;
	idr->count++;
	spin_unlock_irqrestore(&idr->lock, flags);
	return 0;
}

void *idr_find(struct idr *idr, u32 id)
{
	if (!idr)
		return 0;
	long slot = idr_slot_of(idr, id);
	if (slot < 0)
		return 0;
	u64 flags;
	spin_lock_irqsave(&idr->lock, &flags);
	void *p = ((u32)slot < idr->capacity) ? idr->slots[slot] : 0;
	spin_unlock_irqrestore(&idr->lock, flags);
	return p;
}

void *idr_remove(struct idr *idr, u32 id)
{
	if (!idr)
		return 0;
	long slot = idr_slot_of(idr, id);
	if (slot < 0)
		return 0;
	u64 flags;
	spin_lock_irqsave(&idr->lock, &flags);
	void *p = 0;
	if ((u32)slot < idr->capacity && idr->slots[slot]) {
		p = idr->slots[slot];
		idr->slots[slot] = 0;
		idr->count--;
		/* Reuse the freed slot next: keeps a churning handle table dense. */
		if ((u32)slot < idr->hint)
			idr->hint = (u32)slot;
	}
	spin_unlock_irqrestore(&idr->lock, flags);
	return p;
}

u32 idr_count(struct idr *idr)
{
	if (!idr)
		return 0;
	u64 flags;
	spin_lock_irqsave(&idr->lock, &flags);
	u32 n = idr->count;
	spin_unlock_irqrestore(&idr->lock, flags);
	return n;
}

int idr_for_each(struct idr *idr, int (*fn)(u32 id, void *ptr, void *data),
                 void *data)
{
	if (!idr || !fn)
		return 0;
	int visited = 0;
	/* Snapshot each entry under the lock and run the callback outside it, so a
	 * callback that sleeps (a fence wait, a firmware load) does not do so with
	 * interrupts disabled. */
	for (u32 i = 0;; i++) {
		u64 flags;
		spin_lock_irqsave(&idr->lock, &flags);
		if (i >= idr->capacity) {
			spin_unlock_irqrestore(&idr->lock, flags);
			break;
		}
		void *p = idr->slots[i];
		spin_unlock_irqrestore(&idr->lock, flags);
		if (!p)
			continue;
		visited++;
		if (fn(idr->base + i, p, data))
			break;
	}
	return visited;
}
