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

/* ── the P-states ACPI declared (the acpi-cpufreq driver) ─────────────────
 *
 * Zero states on a machine whose driver is not acpi-cpufreq: HWP and the
 * bus-ratio request describe a window, not a list, and inventing a list for
 * them would be this kernel's arithmetic dressed as the platform's data.
 *
 * `cpufreq_state_khz` and `_control` are what the firmware declared for one
 * state: the frequency, and the value written to the register `_PCT` names to
 * ask for it. `cpufreq_request_took` says whether that write was accepted —
 * a guest whose hypervisor refuses the register reports the refusal rather
 * than a frequency change that did not happen. */
int cpufreq_state_count(void);
u32 cpufreq_state_khz(int idx);
u32 cpufreq_state_control(int idx);
/* The namespace path whose _PSS these came from, "" when there is none. */
const char *cpufreq_pss_path(void);
/* "fixed-hw" (the IA32_PERF_CTL pair), "system-io", "unsupported", "none". */
const char *cpufreq_pct_space_name(void);
/* Ask for one state by index (0 is the fastest, as _PSS is ordered). */
int cpufreq_request_state(int idx);
int cpufreq_selected_state(void);
int cpufreq_request_took(void);
/* The status register's value now, or 0 when it will not answer. */
u32 cpufreq_status_value(void);

#endif
