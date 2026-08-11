/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_STOP_MACHINE_H
#define LKPI_LINUX_STOP_MACHINE_H
#include <linux/types.h>
/*
 * Running a function with every other CPU parked.
 *
 * i915 uses it in one place — swapping the GGTT under a reset — where the
 * guarantee is the point. b1nix has no such primitive, and running the callback
 * on the current CPU while the others keep going would be the wrong answer
 * dressed as the right one. So it refuses with -ENOSYS: the caller reports the
 * operation as unavailable rather than corrupting a page table under another
 * CPU's feet.
 */
struct cpumask;
int stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus);
#endif
