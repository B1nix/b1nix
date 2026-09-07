/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the reader/writer semaphore, out of line.
 *
 * Out of line for the same reason the spinlock is (see kernel/lkpi/lock.c): the
 * header must stay free of b1nix declarations, because the translation units
 * that include it are compiling imported filesystem source in which
 * `spinlock_t` and `spin_lock` already mean something else.
 *
 * The wait channel is the semaphore's own address. Readers and writers park on
 * the same one and every release wakes all of them; each re-tests and the ones
 * that still cannot proceed park again. That is more wakeups than Linux issues,
 * never fewer, which is the property that matters — a waiter whose turn came
 * cannot be left asleep.
 */

#include <lkpi/env.h>
#include <b1nix/arch.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <lkpi/rwsem.h>

_Static_assert(sizeof(spinlock_t) == sizeof(int),
               "lkpi_rwsem's guard word must match b1nix's spinlock_t");

static usize rwsem_current_id(void)
{
	struct task *t = current_task;
	return t ? t->id + 1 : 0;
}

/* One pass of the cannot-park path: early boot and interrupt context have no
 * task to park. Imported code taking a rwsem there is a bug, but hanging the
 * machine is a worse way to report it than making progress. */
static void rwsem_spin(void)
{
	/* cpu_relax() rather than a `pause` written out here: this tree builds for
	 * aarch64 as well, where the instruction is `yield` and `pause` does not
	 * assemble at all. <b1nix/arch.h> is where that choice already lives. */
	cpu_relax();
	tlb_shootdown_poll();
}

void lkpi_rwsem_init(struct lkpi_rwsem *s)
{
	if (!s)
		return;
	s->readers = 0;
	s->writer = 0;
	s->writers_waiting = 0;
	s->owner = 0;
	s->guard = SPINLOCK_INIT;
}

int lkpi_rwsem_down_read_trylock(struct lkpi_rwsem *s)
{
	if (!s)
		return 0;
	u64 flags;
	int got = 0;
	spin_lock_irqsave(&s->guard, &flags);
	/* Defer to a waiting writer. Without this a steady stream of readers
	 * never lets one in, and for a filesystem that is a transaction commit
	 * that never runs. */
	if (!s->writer && s->writers_waiting == 0) {
		s->readers++;
		got = 1;
	}
	spin_unlock_irqrestore(&s->guard, flags);
	return got;
}

void lkpi_rwsem_down_read(struct lkpi_rwsem *s)
{
	if (!s)
		return;
	for (;;) {
		if (lkpi_rwsem_down_read_trylock(s))
			return;
		if (!scheduler_can_block()) {
			rwsem_spin();
			continue;
		}
		/* Two-phase wait: publish on the channel, re-test under the guard,
		 * park only if the answer is still no. A release landing between the
		 * test and the park is therefore already visible to the re-test. */
		scheduler_wait_prepare(s);
		u64 flags;
		spin_lock_irqsave(&s->guard, &flags);
		int blocked = s->writer || s->writers_waiting != 0;
		spin_unlock_irqrestore(&s->guard, flags);
		if (blocked)
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

void lkpi_rwsem_up_read(struct lkpi_rwsem *s)
{
	if (!s)
		return;
	u64 flags;
	int last = 0;
	spin_lock_irqsave(&s->guard, &flags);
	if (s->readers > 0)
		s->readers--;
	last = (s->readers == 0);
	spin_unlock_irqrestore(&s->guard, flags);
	/* Only the reader that emptied the semaphore can have unblocked a writer;
	 * waking on every up_read would be a thundering herd on a hot inode. */
	if (last)
		scheduler_wake_all(s);
}

int lkpi_rwsem_down_write_trylock(struct lkpi_rwsem *s)
{
	if (!s)
		return 0;
	u64 flags;
	int got = 0;
	spin_lock_irqsave(&s->guard, &flags);
	if (!s->writer && s->readers == 0) {
		s->writer = 1;
		s->owner = rwsem_current_id();
		got = 1;
	}
	spin_unlock_irqrestore(&s->guard, flags);
	return got;
}

void lkpi_rwsem_down_write(struct lkpi_rwsem *s)
{
	if (!s)
		return;
	u64 flags;

	/* Announce the intent before the first attempt, not after a failed one:
	 * the counter is what holds new readers back, and a writer that raised it
	 * only on the slow path would be overtaken by every reader that arrived
	 * while it was trying. */
	spin_lock_irqsave(&s->guard, &flags);
	s->writers_waiting++;
	spin_unlock_irqrestore(&s->guard, flags);

	for (;;) {
		spin_lock_irqsave(&s->guard, &flags);
		int got = (!s->writer && s->readers == 0);
		if (got) {
			s->writer = 1;
			s->owner = rwsem_current_id();
			s->writers_waiting--;
		}
		spin_unlock_irqrestore(&s->guard, flags);
		if (got)
			return;

		if (!scheduler_can_block()) {
			rwsem_spin();
			continue;
		}
		scheduler_wait_prepare(s);
		spin_lock_irqsave(&s->guard, &flags);
		int blocked = s->writer || s->readers != 0;
		spin_unlock_irqrestore(&s->guard, flags);
		if (blocked)
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

void lkpi_rwsem_up_write(struct lkpi_rwsem *s)
{
	if (!s)
		return;
	u64 flags;
	spin_lock_irqsave(&s->guard, &flags);
	s->writer = 0;
	s->owner = 0;
	spin_unlock_irqrestore(&s->guard, flags);
	scheduler_wake_all(s);
}

void lkpi_rwsem_downgrade_write(struct lkpi_rwsem *s)
{
	if (!s)
		return;
	u64 flags;
	spin_lock_irqsave(&s->guard, &flags);
	/* The read reference is taken in the same critical section that drops the
	 * write one. Releasing and re-acquiring would let a writer in between, and
	 * the caller's whole reason for downgrading is that it must not. */
	s->writer = 0;
	s->owner = 0;
	s->readers++;
	spin_unlock_irqrestore(&s->guard, flags);
	scheduler_wake_all(s);
}

int lkpi_rwsem_is_locked(const struct lkpi_rwsem *s)
{
	if (!s)
		return 0;
	return s->writer != 0 || s->readers > 0;
}

int lkpi_rwsem_is_write_owner(const struct lkpi_rwsem *s)
{
	if (!s)
		return 0;
	return s->writer != 0 && s->owner == rwsem_current_id();
}

/* ── counting semaphore ─────────────────────────────────────────── */

void lkpi_sema_init(struct lkpi_semaphore *s, int count)
{
	if (!s)
		return;
	s->count = count;
	s->guard = SPINLOCK_INIT;
}

int lkpi_sema_trydown(struct lkpi_semaphore *s)
{
	if (!s)
		return 1;
	u64 flags;
	int got = 0;
	spin_lock_irqsave(&s->guard, &flags);
	if (s->count > 0) {
		s->count--;
		got = 1;
	}
	spin_unlock_irqrestore(&s->guard, flags);
	return got ? 0 : 1;
}

void lkpi_sema_down(struct lkpi_semaphore *s)
{
	if (!s)
		return;
	for (;;) {
		if (lkpi_sema_trydown(s) == 0)
			return;
		if (!scheduler_can_block()) {
			rwsem_spin();
			continue;
		}
		scheduler_wait_prepare(s);
		u64 flags;
		spin_lock_irqsave(&s->guard, &flags);
		int empty = (s->count <= 0);
		spin_unlock_irqrestore(&s->guard, flags);
		if (empty)
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

void lkpi_sema_up(struct lkpi_semaphore *s)
{
	if (!s)
		return;
	u64 flags;
	spin_lock_irqsave(&s->guard, &flags);
	s->count++;
	spin_unlock_irqrestore(&s->guard, flags);
	scheduler_wake_all(s);
}
