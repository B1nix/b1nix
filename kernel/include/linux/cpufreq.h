/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_CPUFREQ_H
#define LKPI_LINUX_CPUFREQ_H
#include <linux/types.h>
/*
 * CPU frequency scaling. b1nix does not scale: the CPU runs at whatever the
 * firmware left it at. A driver asking for the current frequency — i915 does,
 * to decide how hard to spin waiting on the GPU — is told 0, which upstream
 * already treats as "unknown" and handles.
 */
static inline unsigned int cpufreq_get(unsigned int cpu) { (void)cpu; return 0; }
static inline unsigned int cpufreq_quick_get(unsigned int cpu) { (void)cpu; return 0; }
static inline unsigned int cpufreq_quick_get_max(unsigned int cpu) { (void)cpu; return 0; }

/* The cpufreq policy for a CPU. b1nix does not scale CPU frequency, so there is
 * no policy object; callers use it to bound GPU frequency against CPU frequency
 * and fall back to the GPU's own limits when it is absent. */
struct cpufreq_policy;
static inline struct cpufreq_policy *cpufreq_cpu_get(unsigned int cpu)
{ (void)cpu; return NULL; }
static inline void cpufreq_cpu_put(struct cpufreq_policy *policy)
{ (void)policy; }


/* What a policy would describe. No policy object is ever produced here — see
 * cpufreq_cpu_get() above — so these members exist to let a caller's field
 * reads compile, not to be read. */
struct cpufreq_cpuinfo { unsigned int max_freq; unsigned int min_freq; };
struct cpufreq_policy {
	struct cpufreq_cpuinfo cpuinfo;
	unsigned int cpu;
	unsigned int cur;
	unsigned int max;
	unsigned int min;
};

#endif
