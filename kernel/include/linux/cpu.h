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
};

static inline int cpuhp_setup_state_nocalls(enum cpuhp_state state,
					    const char *name,
					    int (*startup)(unsigned int cpu),
					    int (*teardown)(unsigned int cpu))
{ (void)state; (void)name; (void)startup; (void)teardown; return 0; }

#endif
