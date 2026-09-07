/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CPUMASK_H
#define LKPI_LINUX_CPUMASK_H

#include <linux/smp.h>
#include <lkpi/env.h>

/*
 * CPU counts and masks.
 *
 * b1nix has no CPU hotplug, so online, possible and present are the same set
 * and the count never changes after boot. What a filesystem actually uses these
 * for is sizing: btrfs sizes its worker thread pools from `num_online_cpus`,
 * and ext4 its per-CPU allocation preferences. A wrong answer here is a
 * performance decision, not a correctness one — but a zero would be a division
 * by zero, so the count is never allowed to be one.
 */

static inline unsigned int num_possible_cpus(void) { return lkpi_cpu_count(); }
static inline unsigned int num_present_cpus(void) { return lkpi_cpu_count(); }
static inline unsigned int nr_cpu_ids_val(void) { return lkpi_cpu_count(); }

#define nr_cpu_ids (nr_cpu_ids_val())
#define NR_CPUS 64

struct cpumask { unsigned long bits[(NR_CPUS + 63) / 64]; };
typedef struct cpumask cpumask_t;

#define for_each_possible_cpu(cpu) \
	for ((cpu) = 0; (cpu) < (int)num_possible_cpus(); (cpu)++)
#define for_each_online_cpu(cpu) for_each_possible_cpu(cpu)

#endif
