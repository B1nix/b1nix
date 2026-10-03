// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 KVM's start-up on b1nix (M131), the architecture's half of
 * kvm_lkpi_init (kvm_sched.c): the early parameters, what the CPU is and
 * the alternatives that depend on it, then the GIC and timer description
 * Linux's drivers would have handed KVM, then kvm_arm_init itself.
 *
 * KVM needs the kernel at EL2 with VHE (boot.S stays there when the CPU has
 * it); without that it declines, as upstream does when HYP mode is
 * unavailable, and b1nix runs on without it.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/psci.h>
#include <linux/arm-smccc.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/percpu.h>
#include <b1nix/kvm_bridge.h>

int lkpi_arm64_cpu_init(void);
int lkpi_arm64_apply_alternatives(void);
int lkpi_arm64_gic_kvm_info(void);
void lkpi_arm64_irq_set_trigger(unsigned int irq, u32 trigger);
int lkpi_initcall_kvm_arm_init(void);

/* The host's firmware interface is b1nix's own (PSCI for CPU bring-up in
 * smp.c); KVM reads these only to hand them to an nVHE hypervisor. */
struct psci_operations psci_ops;

struct psci_0_1_function_ids get_psci_0_1_function_ids(void)
{
	struct psci_0_1_function_ids ids = { 0 };

	return ids;
}

u32 arm_smccc_get_version(void)
{
	return ARM_SMCCC_VERSION_1_0;
}

int kvm_arch_lkpi_early(void)
{
	int r;

	if (!b1nix_kvm_at_el2()) {
		pr_info("kvm: the kernel runs at EL1, no hypervisor mode\n");
		return -ENODEV;
	}
	/* KVM's per-CPU copies (kvm_percpu_init, already done), as the offsets
	 * its hyp assembly adds to a per-CPU symbol. */
	for (int cpu = 0; cpu < b1nix_kvm_cpu_count() && cpu < NR_CPUS; cpu++)
		if (kvm_percpu_base[cpu])
			b1nix_kvm_set_percpu_off(cpu, (u64)(kvm_percpu_base[cpu] -
							    __start_kvm_percpu));
	lkpi_run_early_params();
	r = lkpi_arm64_cpu_init();
	if (r)
		return r;
	return lkpi_arm64_apply_alternatives();
}

int kvm_arch_lkpi_start(void)
{
	int r;

	r = lkpi_arm64_gic_kvm_info();
	if (r) {
		pr_info("kvm: no GICv3 with a maintenance interrupt (%d)\n", r);
		return r;
	}
	lkpi_arm64_irq_set_trigger(b1nix_kvm_timer_irq(2), b1nix_kvm_timer_irq_flags(2));
	lkpi_arm64_irq_set_trigger(b1nix_kvm_timer_irq(1), b1nix_kvm_timer_irq_flags(1));
	r = lkpi_initcall_kvm_arm_init();
	if (!r)
		pr_info("kvm: arm64 VHE\n");
	return r;
}
