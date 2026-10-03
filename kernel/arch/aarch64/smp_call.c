// SPDX-License-Identifier: GPL-2.0-only
/*
 * Run a function on another CPU (M131: KVM loads and unloads per-CPU state
 * this way), the aarch64 side of <b1nix/ipi.h>. One request slot per target
 * CPU, raised with the call SGI; the target runs it from its interrupt
 * handler, or from a spin loop of its own that polls (smp_call_poll_pending)
 * while it has interrupts off. Same protocol as kernel/arch/x86_64/smp_call.c.
 * Needs a GICv3: the v2 path sends no SGIs.
 */
#include <b1nix/ipi.h>
#include <b1nix/arch.h>
#include <b1nix/gicv3.h>
#include <b1nix/lapic.h>
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

void smp_call_poll_pending(void)
{
	if (__atomic_load_n(&g_call[this_cpu()].state, __ATOMIC_ACQUIRE) == CALL_REQUESTED)
		smp_call_run_mine();
}

/* Waiting on another CPU: run whatever it (or a third) asked of this one
 * meanwhile, so two CPUs calling each other with interrupts off cannot
 * wait on each other for ever. */
static void smp_call_poll(void)
{
	smp_call_poll_pending();
	cpu_relax();
}

int smp_call_on_cpu(int cpu, void (*fn)(void *), void *info, int wait)
{
	struct call_slot *s;
	u64 mpidr, gen;

	if (cpu < 0 || cpu >= MAX_CPUS || !fn)
		return -EINVAL;
	if (cpu == this_cpu()) {
		u64 flags = interrupts_save();

		fn(info);
		interrupts_restore(flags);
		return 0;
	}
	mpidr = aarch64_cpu_mpidr((u32)cpu);
	if (!get_percpu_n(cpu) || !mpidr || !gicv3_present())
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
	gicv3_send_sgi(mpidr, GICV3_SGI_CALL);
	if (wait)
		while (__atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) == gen)
			smp_call_poll();
	/* A caller that does not wait still cannot have its request
	 * overwritten: the next sender waits for the slot to go idle. */
	__sync_lock_release(&s->owner);
	return 0;
}

void smp_call_handler(void)
{
	smp_call_run_mine();
}
