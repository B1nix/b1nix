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

#include <b1nix/arch.h>
#include <lkpi/env.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <lkpi/lock.h>

_Static_assert(sizeof(spinlock_t) == sizeof(int),
               "lkpi_spinlock's raw word must match b1nix's spinlock_t");

/* "Taken from a context with no current task" -- an interrupt handler, or
 * before the scheduler exists. Zero cannot say that: the boot task's id IS
 * zero, so a report naming "task 0" left the two indistinguishable, and the
 * one that matters (an interrupt that never released) reads as the other. */
#define LKPI_LOCK_NO_TASK ((u64)~0ull)

/* How many of these locks this CPU holds.
 *
 * A task that gives up the CPU while holding one leaves every other CPU -- and
 * itself, once something else runs here -- spinning on a lock whose owner is
 * not running. That is the deadlock i915's register read produced roughly one
 * boot in five: it takes uncore->lock, and something under it yielded.
 *
 * The count is per CPU rather than per task because these locks are held with
 * interrupts off, so the holder can neither migrate nor be preempted; whoever
 * is running on this CPU is the holder. */
static int lkpi_locks_held[MAX_CPUS];
/* Where this CPU's outermost lock was taken, and which lock it is. Kept so a
 * violation can name the acquire that is still outstanding rather than only
 * the code that tripped over it. */
static u64 lkpi_locks_entry_site[MAX_CPUS];
static const void *lkpi_locks_entry_lock[MAX_CPUS];
static u64 lkpi_locks_entry_task[MAX_CPUS];
/* Off until the first lkpi lock is taken. Before that the check would run on
 * every interrupts_enable() in the whole kernel for nothing, including early
 * boot where percpu is not yet the CPU's own. */
static int lkpi_lock_check_armed;

void lkpi_lock_report_held(void);

/* Called from interrupts_enable() and from interrupts_restore() when the state
 * being restored has interrupts on. Silent unless this CPU holds one of these
 * locks -- which it must not, since every acquire masked them.
 *
 * The unlock path decrements the count BEFORE restoring, so releasing the last
 * lock does not trip this; a nested unlock that restores an outer acquire's
 * "interrupts were on" while an inner lock is still held does, which is the
 * shape being hunted. */
void lkpi_lock_irq_on_check(u64 site)
{
	u32 cpu;

	if (!lkpi_lock_check_armed)
		return;
	cpu = percpu_read(cpu_id);
	if (cpu >= MAX_CPUS || lkpi_locks_held[cpu] == 0)
		return;

	console_write("\nLKPI: interrupts turned on with ");
	console_write_dec((u64)lkpi_locks_held[cpu]);
	console_write(" spinlock(s) held, from 0x");
	console_write_hex64(site);
	ksym_print(site);
	console_write("\n");
	lkpi_lock_report_held();
	panic("interrupts enabled while holding an lkpi spinlock");
}

void lkpi_lock_report_held(void)
{
	u32 cpu = percpu_read(cpu_id);

	if (cpu >= MAX_CPUS)
		return;
	console_write("  lkpi locks held on cpu ");
	console_write_dec(cpu);
	console_write(": ");
	console_write_dec((u64)lkpi_locks_held[cpu]);
	console_write(", outermost lock 0x");
	console_write_hex64((u64)(usize)lkpi_locks_entry_lock[cpu]);
	console_write(" taken at 0x");
	console_write_hex64(lkpi_locks_entry_site[cpu]);
	ksym_print(lkpi_locks_entry_site[cpu]);
	/* Whose lock it is, against who is running here now. A CPU whose id is not
	 * yet its own -- an AP still using the boot CPU's percpu block -- would
	 * account its locks to cpu 0 and make this count belong to two CPUs at
	 * once, which is a report to disbelieve rather than a bug to chase. */
	console_write(" by task ");
	console_write_dec(lkpi_locks_entry_task[cpu]);
	console_write(", running here now: task ");
	console_write_dec(current_task ? (u64)current_task->id : LKPI_LOCK_NO_TASK);
	console_write("\n");
}

int lkpi_holding_spinlock(void)
{
	u32 cpu = percpu_read(cpu_id);

	return cpu < MAX_CPUS ? lkpi_locks_held[cpu] : 0;
}

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
	u64 asker = current_task ? (u64)current_task->id : LKPI_LOCK_NO_TASK;
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
	if (l->raw != 0 && l->owner_cpu == cpu && asker != LKPI_LOCK_NO_TASK &&
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
	l->owner_task = current_task ? (u64)current_task->id : LKPI_LOCK_NO_TASK;
	if ((u32)cpu < MAX_CPUS && lkpi_locks_held[cpu]++ == 0) {
		lkpi_locks_entry_site[cpu] = l->acquired_at;
		lkpi_locks_entry_lock[cpu] = l;
		lkpi_locks_entry_task[cpu] = l->owner_task;
		lkpi_lock_check_armed = 1;
	}
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
	/* interrupts_save/restore are the tree's per-arch spelling of this — the
	 * old #else arm was the dead 32-bit x86 path, which on aarch64 became a
	 * `pushfd` the assembler rejects. */
	u64 f = interrupts_save();

	if (__atomic_exchange_n(&l->raw, 1, __ATOMIC_ACQUIRE) != 0) {
		interrupts_restore(f);
		return 0;
	}
	l->flags = f;
	l->acquired_at = (u64)(usize)__builtin_return_address(0);
	l->owner_cpu = (int)percpu_read(cpu_id);
	if ((u32)l->owner_cpu < MAX_CPUS && lkpi_locks_held[l->owner_cpu]++ == 0) {
		lkpi_locks_entry_site[l->owner_cpu] = l->acquired_at;
		lkpi_locks_entry_lock[l->owner_cpu] = l;
		lkpi_locks_entry_task[l->owner_cpu] = l->owner_task;
	}
	/* Record the owner here too. Leaving it alone kept whatever task last held
	 * the lock through a blocking acquire, so a lockup report on a lock taken
	 * by trylock named a task that had let go of it long before -- and the
	 * recursion check below compares against this field. */
	l->owner_task = current_task ? (u64)current_task->id : LKPI_LOCK_NO_TASK;
	return 1;
}

void lkpi_spin_unlock(struct lkpi_spinlock *l)
{
	if (!l)
		return;
	u64 f = l->flags;
	int cpu = l->owner_cpu;

	/* Releasing a lock this CPU did not take is what would make the count lie,
	 * and a count that lies is what made two attempted fixes for the probe
	 * hang misfire. It cannot happen legitimately: these locks are held with
	 * interrupts off, so the holder can neither migrate nor be preempted. Say
	 * so loudly at the moment it happens rather than let the consequence show
	 * up somewhere else. */
	/* Reported once, not fatal.
	 *
	 * It is a real violation -- a lock these acquires take with interrupts off
	 * cannot legitimately be released on another CPU, so the task migrated
	 * while holding it. It fires early in a passthrough boot, from a lock
	 * taken inside scheduler_yield's own prologue (the reapers walk the
	 * filesystem, which takes linuxkpi locks), and panicking there ends every
	 * run before the thing being investigated happens. The count it corrupts
	 * is a diagnostic, so the honest trade is to say so and carry on. */
	static unsigned reported_wrong_cpu;

	if (cpu >= 0 && (u32)cpu < MAX_CPUS && cpu != (int)percpu_read(cpu_id) &&
	    reported_wrong_cpu++ == 0) {
		console_write("\nLKPI SPINLOCK released on the wrong cpu: lock 0x");
		console_write_hex64((u64)(usize)l);
		console_write(" taken on cpu ");
		console_write_dec((u64)cpu);
		console_write(" at 0x");
		console_write_hex64(l->acquired_at);
		ksym_print(l->acquired_at);
		console_write(", released on cpu ");
		console_write_dec((u64)percpu_read(cpu_id));
		console_write("\n");
	}
	if ((u32)cpu < MAX_CPUS && lkpi_locks_held[cpu] > 0)
		lkpi_locks_held[cpu]--;
	l->flags = 0;
	l->owner_cpu = -1;
	l->owner_task = 0;
	l->acquired_at = 0;
	/*
	 * Each lock restores the interrupt state its own acquire saw.
	 *
	 * Holding them off until this CPU releases its LAST lkpi lock was tried,
	 * on the theory that an outer lock taken with interrupts on turns them
	 * back on while an inner one is still held. It made the i915 probe hang
	 * MORE likely, not less (2 boots in 6 against 0 in 6), because the count
	 * it keys on is per CPU and a lock released on a different CPU than it was
	 * taken on leaves that count positive for good -- and then this path never
	 * restores interrupts at all. The count is kept for the report below,
	 * which is honest about not knowing which of the two it is looking at.
	 */
	spin_unlock_irqrestore((spinlock_t *)&l->raw, f);
	if (lkpi_irqs_enabled())
		lkpi_note_irq_on();
}
