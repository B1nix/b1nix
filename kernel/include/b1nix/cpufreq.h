/* SPDX-License-Identifier: GPL-2.0-only */
/* Frequency scaling (M129). See kernel/dev/cpufreq.c: HWP where the processor
 * has it, the older explicit ratio request where it does not, and an honest
 * "none" on a machine (or a hypervisor) that offers neither. */
#ifndef B1NIX_CPUFREQ_H
#define B1NIX_CPUFREQ_H

#include <b1nix/types.h>

void cpufreq_init(void);
/* "hwp", "acpi-perf" or "none" — what /sys calls scaling_driver. */
const char *cpufreq_driver_name(void);
/* The frequency in force, in kHz, or 0 when the processor will not say (the
 * caller then falls back to the clock the TSC calibration measured). */
u32 cpufreq_cur_khz(void);
u32 cpufreq_max_khz(void);
u32 cpufreq_min_khz(void);
const char *cpufreq_governor(void);
/* "performance" or "powersave"; anything else, or no driver, is refused. */
int cpufreq_set_governor(const char *name);

#endif
