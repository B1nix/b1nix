/*
 * SPDX-License-Identifier: GPL-2.0-only
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

#include <lkpi/env.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
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
	l->acquired_at = 0;
	l->owner_cpu = -1;
}

void lkpi_spin_lock(struct lkpi_spinlock *l)
{
	if (!l)
		return;

	/*
	 * Catch the recursive acquire before spinning on it. Waiting would hang
	 * this CPU forever with no way left to say why: the holder is us, and the
	 * address that matters is where we took it the first time, which is
	 * recorded below and would otherwise be overwritten.
	 */
	int cpu = (int)percpu_read(cpu_id);
	u64 asker = current_task ? (u64)current_task->id : 0;
	/*
	 * A matching CPU id is not evidence of recursion, and treating it as such
	 * cost several runs to a panic that described something that cannot happen:
	 * the holder took the lock with interrupts already off, so it can be neither
	 * preempted nor migrated, and no other task can be running on its CPU. What
	 * the report showed instead was holder-task 0 (taken from a context with no
	 * current task) and a different asking task — one of the two CPU ids was
	 * simply not the CPU the code was on.
	 *
	 * Recursion means the same THREAD asks twice, so that is what is tested. A
	 * genuine deadlock between two tasks still gets reported, by the acquire
	 * loop's own stuck detector, with the same detail and without inventing a
	 * cause.
	 */
	if (l->raw != 0 && l->owner_cpu == cpu && asker != 0 &&
	    l->owner_task == asker) {

		console_write("\nLKPI SPINLOCK RECURSION on cpu ");
		console_write_dec((u64)cpu);
		console_write(" holder-task ");
		console_write_dec(l->owner_task);
		console_write(" asking-task ");
		console_write_dec(asker);
		console_write(": lock=0x");
		console_write_hex64((u64)(usize)l);
		console_write("\n  already held from: 0x");
		console_write_hex64(l->acquired_at);
		ksym_print(l->acquired_at);
		console_write("\n  re-acquired from:  0x");
		u64 here = (u64)(usize)__builtin_return_address(0);
		console_write_hex64(here);
		ksym_print(here);
		/*
		 * Scan the stack for anything that looks like a return address into
		 * kernel text. The imported objects are built without frame pointers,
		 * so there is no frame chain to walk — this over-reports (a stale
		 * value left in a dead slot looks the same as a live return address)
		 * but it is the only way to see who called the locking read, and a
		 * plausible-but-dead frame is easy to discount by eye.
		 */
		extern char __kernel_text_start[], __kernel_text_end[];
		u64 lo = (u64)(usize)__kernel_text_start;
		u64 hi = (u64)(usize)__kernel_text_end;
		const u64 *sp = (const u64 *)(usize)&here;
		console_write("\n  stack (possible return addresses):");
		for (int i = 0, shown = 0; i < 256 && shown < 12; i++) {
			u64 v = sp[i];
			if (v < lo || v >= hi)
				continue;
			console_write("\n    0x");
			console_write_hex64(v);
			ksym_print(v);
			shown++;
		}
		console_write("\n");
		panic("lkpi spinlock recursive acquire");
	}

	u64 f;
	/* Note only the transition. A lock taken while interrupts are already off
	 * did not turn them off, and recording it there buries the site that did. */
	int was_on = lkpi_irqs_enabled();

	spin_lock_irqsave((spinlock_t *)&l->raw, &f);
	l->flags = f;
	if (was_on)
		lkpi_note_irq_off((u64)(usize)__builtin_return_address(0));
	l->acquired_at = (u64)(usize)__builtin_return_address(0);
	l->owner_cpu = cpu;
	l->owner_task = current_task ? (u64)current_task->id : 0;
}

int lkpi_spin_trylock(struct lkpi_spinlock *l)
{
	if (!l)
		return 0;

	/*
	 * Interrupts go off before the attempt, not after: if they were left on
	 * and an interrupt handler took the same lock on this CPU between the
	 * exchange and the disable, the handler would spin on a lock this CPU
	 * holds. On failure they are restored, because we are not returning as
	 * the holder.
	 */
	u64 f;
#ifdef __x86_64__
	__asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
#else
	u32 f32;
	__asm__ volatile("pushfd; popl %0; cli" : "=r"(f32) : : "memory");
	f = f32;
#endif
	if (__atomic_exchange_n(&l->raw, 1, __ATOMIC_ACQUIRE) != 0) {
#ifdef __x86_64__
		__asm__ volatile("pushq %0; popfq" : : "r"(f) : "memory");
#else
		u32 r32 = (u32)f;
		__asm__ volatile("pushl %0; popfd" : : "r"(r32) : "memory");
#endif
		return 0;
	}
	l->flags = f;
	l->acquired_at = (u64)(usize)__builtin_return_address(0);
	l->owner_cpu = (int)percpu_read(cpu_id);
	return 1;
}

void lkpi_spin_unlock(struct lkpi_spinlock *l)
{
	if (!l)
		return;
	u64 f = l->flags;
	l->flags = 0;
	l->owner_cpu = -1;
	l->owner_task = 0;
	l->acquired_at = 0;
	spin_unlock_irqrestore((spinlock_t *)&l->raw, f);
	if (lkpi_irqs_enabled())
		lkpi_note_irq_on();
}
