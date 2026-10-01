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
/* One of cpufreq_governors(); anything else, or no driver, is refused. */
int cpufreq_set_governor(const char *name);
/* The governors this driver can honour, space-separated, as
 * scaling_available_governors lists them. */
const char *cpufreq_governors(void);

/* ── the policy (M135) ────────────────────────────────────────────────────
 *
 * The window a governor chooses in: scaling_min_freq and scaling_max_freq as
 * written, capped by the platform's _PPC. cpufreq_max_khz/min_khz stay what
 * the processor can do (cpuinfo_*_freq). */
u32 cpufreq_policy_max_khz(void);
u32 cpufreq_policy_min_khz(void);
int cpufreq_set_policy_limit(int is_max, u32 khz);
/* The userspace governor's frequency; refused under any other governor. */
int cpufreq_set_speed(u32 khz);
u32 cpufreq_setspeed_khz(void);
/* _PPC: the index of the fastest state the platform allows now, and its
 * frequency (bios_limit); -1 and 0 without the ACPI driver. */
int cpufreq_ppc(void);
u32 cpufreq_bios_limit_khz(void);
/* Re-read _PPC: Notify(processor, 0x80) on the node the states came from. */
void cpufreq_ppc_changed(void);
int cpufreq_is_pss_node(const char *path);
/* Passive cooling's ceiling: 0 is none, k caps the clock at the k-th _PSS
 * state, up to cpufreq_thermal_states(). */
int cpufreq_thermal_states(void);
int cpufreq_thermal_limit(void);
int cpufreq_set_thermal_limit(int state);
/* stats/: milliseconds spent in each state, and the number of changes. */
u64 cpufreq_state_time_ms(int idx);
u64 cpufreq_transitions(void);
/* ondemand: the last sampled load (percent of the busiest CPU) and its two
 * tunables, which the setters range-check. */
u32 cpufreq_od_load(void);
u32 cpufreq_od_sampling_rate_us(void);
u32 cpufreq_od_up_threshold(void);
int cpufreq_od_set_sampling_rate_us(u32 us);
int cpufreq_od_set_up_threshold(u32 pct);

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
int cpufreq_selected_state(void);
int cpufreq_request_took(void);
/* The status register's value now, or 0 when it will not answer. */
u32 cpufreq_status_value(void);

#endif
