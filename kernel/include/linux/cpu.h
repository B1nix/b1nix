/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CPU_H
#define LKPI_LINUX_CPU_H
#include <linux/cpumask.h>

/*
 * CPU hotplug callbacks.
 *
 * b1nix brings every CPU up at boot and never takes one offline, so a teardown
 * callback registered here has no event to run on. The _nocalls form does not
 * run the callback for CPUs already up either, so registering is complete as
 * soon as it returns.
 */
enum cpuhp_state {
	CPUHP_RADIX_DEAD,
	CPUHP_AP_KVM_ONLINE,
	CPUHP_AP_X86_KVM_CLK_ONLINE,
};

/* A state whose callbacks run for the CPUs that are up (M131: KVM enables
 * VMX on each through it). CPUs do not come and go here after boot, so the
 * startup callback runs once per online CPU at registration and the teardown
 * once each at removal. Implemented in kernel/lkpi/kvm_smp.c. */
int cpuhp_setup_state(enum cpuhp_state state, const char *name,
		      int (*startup)(unsigned int cpu),
		      int (*teardown)(unsigned int cpu));
void cpuhp_remove_state(enum cpuhp_state state);

static inline int cpuhp_setup_state_nocalls(enum cpuhp_state state,
					    const char *name,
					    int (*startup)(unsigned int cpu),
					    int (*teardown)(unsigned int cpu))
{ (void)state; (void)name; (void)startup; (void)teardown; return 0; }

#endif
