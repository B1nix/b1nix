/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: the spinlock operations, out of line.
 *
 * They live here rather than as inlines in <lkpi/lock.h> so that header needs
 * no b1nix declarations — which is what keeps `spinlock_t` and `spin_lock` from
 * meaning two different things inside a translation unit that is also compiling
 * imported DRM source.
 *
 * The lock word is declared as `volatile int` on the header side and used as a
 * b1nix `spinlock_t` here. The assertion below makes a change to either side a
 * build error instead of a silent layout mismatch.
 */

#include <b1nix/spinlock.h>
#include <lkpi/lock.h>

_Static_assert(sizeof(spinlock_t) == sizeof(int),
               "lkpi_spinlock's raw word must match b1nix's spinlock_t");

void lkpi_spin_lock_init(struct lkpi_spinlock *l)
{
	if (!l)
		return;
	l->raw = SPINLOCK_INIT;
	l->flags = 0;
}

void lkpi_spin_lock(struct lkpi_spinlock *l)
{
	if (!l)
		return;
	u64 f;
	spin_lock_irqsave((spinlock_t *)&l->raw, &f);
	l->flags = f;
}

void lkpi_spin_unlock(struct lkpi_spinlock *l)
{
	if (!l)
		return;
	u64 f = l->flags;
	l->flags = 0;
	spin_unlock_irqrestore((spinlock_t *)&l->raw, f);
}
