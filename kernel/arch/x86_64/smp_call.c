// SPDX-License-Identifier: GPL-2.0-only
/*
 * Running a function on another CPU (M131).
 *
 * Until KVM there was nothing in the kernel that had to execute an
 * instruction on a particular processor: the TLB shootdown and the reschedule
 * IPI are both fixed jobs. A VMCS, though, is loaded on one CPU, and only that
 * CPU can VMCLEAR it -- so when a vCPU's thread moves, the old CPU has to run
 * the clear. This is the general form: one request slot per target CPU, a
 * dedicated vector, and the caller either waits for the function to have run
 * or does not.
 *
 * The function runs on the target with interrupts off, like Linux's
 * smp_call_function_single callbacks. A CPU that waits -- for its own request
 * to be run, or for a slot to free up -- keeps servicing what is aimed at it
 * (its own slot, and TLB shootdowns) while it spins, the same way the TLB
 * shootdown's waiter does: two CPUs that each wait for the other with
 * interrupts off would otherwise never get past each other.
 */
#include <b1nix/ipi.h>
#include <b1nix/lapic.h>
#include <b1nix/arch.h>
#include <b1nix/tlb.h>
#include <b1nix/types.h>
#include <b1nix/errno.h>

enum { CALL_IDLE, CALL_REQUESTED, CALL_RUNNING };

struct call_slot {
	volatile int owner;           /* a sender holds the slot */
	volatile int state;           /* CALL_* */
	void (*volatile fn)(void *);
	void *volatile info;
	volatile u64 gen;             /* bumped each time a request completes */
};

static struct call_slot g_call[MAX_CPUS];

static int this_cpu(void)
{
	struct percpu *p = get_percpu();

	return p ? (int)p->cpu_id : 0;
}

/* Run this CPU's pending request, if any. Exactly one of the IPI and a
 * waiter's poll claims it. */
static void smp_call_run_mine(void)
{
	struct call_slot *s = &g_call[this_cpu()];
	int expect = CALL_REQUESTED;

	if (!__atomic_compare_exchange_n(&s->state, &expect, CALL_RUNNING, 0,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
	s->fn(s->info);
	__atomic_add_fetch(&s->gen, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&s->state, CALL_IDLE, __ATOMIC_RELEASE);
}

/* From every spin-wait (tlb_shootdown_poll): a CPU spinning on a lock with
 * interrupts off still runs what another CPU is waiting on it to run. One
 * load when nothing is asked. */
void smp_call_poll_pending(void)
{
	if (__atomic_load_n(&g_call[this_cpu()].state, __ATOMIC_ACQUIRE) == CALL_REQUESTED)
		smp_call_run_mine();
}

static void smp_call_poll(void)
{
	tlb_shootdown_poll();
	cpu_relax();
}

int smp_call_on_cpu(int cpu, void (*fn)(void *), void *info, int wait)
{
	struct percpu *target;
	struct call_slot *s;
	u64 gen;

	if (cpu < 0 || cpu >= MAX_CPUS || !fn)
		return -EINVAL;
	if (cpu == this_cpu()) {
		u64 flags = interrupts_save();

		fn(info);
		interrupts_restore(flags);
		return 0;
	}
	target = get_percpu_n(cpu);
	if (!target)
		return -ENXIO;
	s = &g_call[cpu];
	/* One request in flight per target; a second sender waits its turn. */
	while (__sync_lock_test_and_set(&s->owner, 1))
		smp_call_poll();
	while (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != CALL_IDLE)
		smp_call_poll();
	s->fn = fn;
	s->info = info;
	gen = __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE);
	__atomic_store_n(&s->state, CALL_REQUESTED, __ATOMIC_RELEASE);
	lapic_send_ipi(target->apic_id, CALL_FUNCTION_VECTOR | LAPIC_ICR_FIXED);
	if (wait)
		while (__atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) == gen)
			smp_call_poll();
	/* A caller that does not wait still cannot have its request
	 * overwritten: the next sender waits for the slot to go idle. */
	__sync_lock_release(&s->owner);
	return 0;
}

/* The CALL_FUNCTION_VECTOR handler. */
void smp_call_handler(void)
{
	lapic_eoi();
	smp_call_run_mine();
}
