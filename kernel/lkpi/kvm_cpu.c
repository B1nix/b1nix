// SPDX-License-Identifier: GPL-2.0-only
/*
 * The CPU-level facilities Linux's KVM expects of its host (M131), the part
 * that is the same on every architecture: real per-CPU storage for KVM's
 * DEFINE_PER_CPU variables, CPU masks and the hotplug-state callbacks,
 * cross-CPU calls, and the system-sleep callbacks. The architecture's own
 * (x86's CPUID, MSRs and APIC; arm64's ID registers and GIC) is
 * kvm_x86_cpu.c and kvm_arm64_*.c.
 *
 * The native half (CPU numbering, IPIs) is kernel/virt/kvm_bridge.c; see
 * <b1nix/kvm_bridge.h>.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cpumask.h>
#include <linux/cpu.h>
#include <linux/syscore_ops.h>
#include <linux/moduleparam.h>
#include <b1nix/kvm_bridge.h>

/* ── per-CPU storage ──────────────────────────────────────────────────── */

/* The dynamic arena lives inside the template, so an alloc_percpu pointer
 * translates exactly like a DEFINE_PER_CPU one. */
#define KVM_PERCPU_ARENA (16 * 1024)
static DEFINE_PER_CPU(char [KVM_PERCPU_ARENA], kvm_percpu_arena);
static unsigned long kvm_percpu_arena_used;
static DEFINE_SPINLOCK(kvm_percpu_lock);

char *kvm_percpu_base[NR_CPUS];

int kvm_percpu_init(void)
{
	usize size = (usize)(__stop_kvm_percpu - __start_kvm_percpu);

	for (int cpu = 0; cpu < b1nix_kvm_cpu_count() && cpu < NR_CPUS; cpu++) {
		if (kvm_percpu_base[cpu])
			continue;
		kvm_percpu_base[cpu] = kzalloc(size, GFP_KERNEL);
		if (!kvm_percpu_base[cpu])
			return -ENOMEM;
		memcpy(kvm_percpu_base[cpu], __start_kvm_percpu, size);
	}
	return 0;
}

void __percpu *__alloc_percpu(size_t size, size_t align)
{
	unsigned long flags, off;
	char *p = NULL;

	if (!align)
		align = 8;
	spin_lock_irqsave(&kvm_percpu_lock, flags);
	off = (kvm_percpu_arena_used + align - 1) & ~(align - 1);
	if (off + size <= KVM_PERCPU_ARENA) {
		p = &kvm_percpu_arena[off];
		kvm_percpu_arena_used = off + size;
	}
	spin_unlock_irqrestore(&kvm_percpu_lock, flags);
	if (!p)
		return NULL;
	/* Zeroed on every CPU, as upstream's allocator returns it. */
	for (int cpu = 0; cpu < b1nix_kvm_cpu_count() && cpu < NR_CPUS; cpu++)
		if (kvm_percpu_base[cpu])
			memset(per_cpu_ptr(p, cpu), 0, size);
	return p;
}

/* The arena is a bump allocator: KVM allocates its per-CPU blocks once at
 * module init and never frees them while the kernel runs. */
void free_percpu(void __percpu *p)
{
	(void)p;
}

/* ── CPUs: masks, liveness, hotplug callbacks, cross calls ────────────── */

bool alloc_cpumask_var(cpumask_var_t *mask, gfp_t flags)
{
	*mask = kzalloc(sizeof(struct cpumask), flags);
	return *mask != NULL;
}

bool zalloc_cpumask_var(cpumask_var_t *mask, gfp_t flags)
{
	return alloc_cpumask_var(mask, flags);
}

void free_cpumask_var(cpumask_var_t mask)
{
	kfree(mask);
}

const struct cpumask *get_cpu_mask(unsigned int cpu)
{
	static struct cpumask masks[NR_CPUS];

	if (cpu >= NR_CPUS)
		cpu = 0;
	masks[cpu].bits[cpu / 64] = 1UL << (cpu % 64);
	return &masks[cpu];
}

bool cpu_online(unsigned int cpu)
{
	return (int)cpu < b1nix_kvm_cpu_count() && b1nix_kvm_cpu_present((int)cpu);
}

/* No hotplug: the possible CPUs are the ones that came up. */
const struct cpumask *lkpi_cpu_possible_mask(void)
{
	static struct cpumask mask;

	for (int cpu = 0; cpu < b1nix_kvm_cpu_count() && cpu < NR_CPUS; cpu++)
		if (b1nix_kvm_cpu_present(cpu))
			mask.bits[cpu / 64] |= 1UL << (cpu % 64);
	return &mask;
}

struct cpuhp_call {
	int (*fn)(unsigned int cpu);
	unsigned int cpu;
	int ret;
};

static void cpuhp_run(void *arg)
{
	struct cpuhp_call *c = arg;

	c->ret = c->fn(c->cpu);
}

static int (*cpuhp_teardown[CPUHP_NR_STATES])(unsigned int cpu);
static int (*cpuhp_startup[CPUHP_NR_STATES])(unsigned int cpu);

/* CPUs do not come and go after boot here: the startup callback runs once on
 * each online CPU now, the teardown once on each at removal. */
int cpuhp_setup_state(enum cpuhp_state state, const char *name,
		      int (*startup)(unsigned int cpu),
		      int (*teardown)(unsigned int cpu))
{
	(void)name;
	if ((unsigned int)state > CPUHP_AP_X86_KVM_CLK_ONLINE)
		return -EINVAL;
	cpuhp_teardown[state] = teardown;
	cpuhp_startup[state] = startup;
	if (!startup)
		return 0;
	for (int cpu = 0; cpu < b1nix_kvm_cpu_count(); cpu++) {
		struct cpuhp_call c = { startup, (unsigned int)cpu, 0 };

		if (!cpu_online(cpu))
			continue;
		b1nix_kvm_call_on_cpu(cpu, cpuhp_run, &c, 1);
		if (c.ret) {
			while (--cpu >= 0) {
				struct cpuhp_call t = { teardown, (unsigned int)cpu, 0 };

				if (teardown && cpu_online(cpu))
					b1nix_kvm_call_on_cpu(cpu, cpuhp_run, &t, 1);
			}
			return c.ret;
		}
	}
	return 0;
}

void cpuhp_remove_state(enum cpuhp_state state)
{
	int (*teardown)(unsigned int cpu);

	if ((unsigned int)state > CPUHP_AP_X86_KVM_CLK_ONLINE)
		return;
	teardown = cpuhp_teardown[state];
	cpuhp_teardown[state] = NULL;
	cpuhp_startup[state] = NULL;
	if (!teardown)
		return;
	for (int cpu = 0; cpu < b1nix_kvm_cpu_count(); cpu++) {
		struct cpuhp_call c = { teardown, (unsigned int)cpu, 0 };

		if (cpu_online(cpu))
			b1nix_kvm_call_on_cpu(cpu, cpuhp_run, &c, 1);
	}
}

/* Around a sleep that parks the CPUs (b1nix has no CPU hotplug): every
 * registered state is taken down on every CPU first, in reverse, and brought
 * up again after, as Linux's hotplug would as each CPU went and came back. */
void kvm_cpuhp_all_down(void)
{
	for (int st = CPUHP_AP_X86_KVM_CLK_ONLINE; st >= 0; st--) {
		if (!cpuhp_teardown[st])
			continue;
		for (int cpu = 0; cpu < b1nix_kvm_cpu_count(); cpu++) {
			struct cpuhp_call c = { cpuhp_teardown[st], (unsigned int)cpu, 0 };

			if (cpu_online(cpu))
				b1nix_kvm_call_on_cpu(cpu, cpuhp_run, &c, 1);
		}
	}
}

void kvm_cpuhp_all_up(void)
{
	for (int st = 0; st <= CPUHP_AP_X86_KVM_CLK_ONLINE; st++) {
		if (!cpuhp_startup[st])
			continue;
		for (int cpu = 0; cpu < b1nix_kvm_cpu_count(); cpu++) {
			struct cpuhp_call c = { cpuhp_startup[st], (unsigned int)cpu, 0 };

			if (cpu_online(cpu))
				b1nix_kvm_call_on_cpu(cpu, cpuhp_run, &c, 1);
		}
	}
}

int smp_call_function_single(int cpu, smp_call_func_t func, void *info, int wait)
{
	return b1nix_kvm_call_on_cpu(cpu, func, info, wait);
}

void smp_call_function_many(const struct cpumask *mask, smp_call_func_t func,
			    void *info, bool wait)
{
	int self = b1nix_kvm_this_cpu();

	/* Every CPU in the mask but this one, as upstream. */
	for (int cpu = 0; cpu < b1nix_kvm_cpu_count(); cpu++)
		if (cpu != self && ((mask->bits[cpu / 64] >> (cpu % 64)) & 1))
			b1nix_kvm_call_on_cpu(cpu, func, info, wait);
}

/* A kick out of guest mode: any interrupt makes the target CPU leave the
 * guest, and the one the bridge sends does nothing else. */
void smp_send_reschedule(int cpu)
{
	b1nix_kvm_kick_cpu(cpu);
}

/* ── system sleep ──────────────────────────────────────────── */

static LIST_HEAD(syscore_list);

void register_syscore_ops(struct syscore_ops *ops)
{
	list_add_tail(&ops->node, &syscore_list);
}

void unregister_syscore_ops(struct syscore_ops *ops)
{
	list_del(&ops->node);
}

int param_get_bool(char *buffer, const struct kernel_param *kp)
{
	(void)buffer; (void)kp;
	return -EINVAL;
}
