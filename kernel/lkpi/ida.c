/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: ida, id allocation without an object.
 *
 * Built on lkpi's idr rather than beside it. An ida is an idr whose stored
 * pointer nobody reads, and keeping one allocator means the id-reuse behaviour
 * — which callers do depend on, since a freed connector index must become
 * available again — is the behaviour that is already tested.
 *
 * The stored pointer is the ida itself rather than NULL: idr treats a NULL
 * pointer as an empty slot, so storing NULL would make every allocated id look
 * free on lookup.
 */

#include <linux/errno.h>
#include <lkpi/idr.h>

/*
 * Built against <lkpi/idr.h>, not <linux/idr.h>: the Linux header redefines
 * idr_alloc as a five-argument macro for imported callers, and this file is on
 * our side of the boundary, so it calls the four-argument function directly.
 */
#include <lkpi/types.h>

static void ida_ensure_init(struct ida *ida)
{
	if (!ida->initialised) {
		idr_init_base(&ida->idr, 0);
		ida->initialised = 1;
	}
}

void ida_init(struct ida *ida)
{
	if (!ida)
		return;
	idr_init_base(&ida->idr, 0);
	ida->initialised = 1;
}

void ida_destroy(struct ida *ida)
{
	if (!ida || !ida->initialised)
		return;
	idr_destroy(&ida->idr);
	ida->initialised = 0;
}

int ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max,
                    gfp_t gfp)
{
	(void)gfp;
	if (!ida)
		return -EINVAL;
	ida_ensure_init(ida);
	int id = idr_alloc(&ida->idr, ida, min, max ? max + 1 : 0);
	return id < 0 ? -ENOSPC : id;
}

int ida_alloc(struct ida *ida, gfp_t gfp)
{
	return ida_alloc_range(ida, 0, 0, gfp);
}

int ida_alloc_max(struct ida *ida, unsigned int max, gfp_t gfp)
{
	return ida_alloc_range(ida, 0, max, gfp);
}

void ida_free(struct ida *ida, unsigned int id)
{
	if (!ida || !ida->initialised)
		return;
	idr_remove(&ida->idr, id);
}
