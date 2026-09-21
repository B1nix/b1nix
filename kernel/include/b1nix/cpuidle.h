/* SPDX-License-Identifier: GPL-2.0-only */
/* Idle states (M129).
 *
 * Every place this kernel parks a CPU goes through cpuidle_enter, so there is
 * one answer to "how does this machine idle" and one set of counters behind
 * /sys/devices/system/cpu/cpuN/cpuidle. What the instruction is — MWAIT into a
 * real C-state, or plain HLT — depends on the processor, and the state list
 * says which it turned out to be rather than what was hoped for. */
#ifndef B1NIX_CPUIDLE_H
#define B1NIX_CPUIDLE_H

#include <b1nix/types.h>

/* Probe the CPU's idle support. Called once, after the console exists. */
void cpuidle_init(void);

/* Park this CPU until an interrupt arrives, in the deepest state it has.
 * Interrupts must be DISABLED on entry: the state is entered with them
 * enabled atomically, so a wake that arrives in the window before the halt is
 * not lost. Returns with interrupts enabled, as the halt leaves them. */
void cpuidle_enter(void);

/* How many idle states this machine has, and what they are. */
int cpuidle_state_count(void);
const char *cpuidle_state_name(int state);
const char *cpuidle_state_desc(int state);
u32 cpuidle_state_latency_us(int state);
/* Per CPU, per state: how many times it was entered and how long was spent in
 * it (microseconds, as sysfs reports). */
u64 cpuidle_state_usage(int cpu, int state);
u64 cpuidle_state_time_us(int cpu, int state);

#endif
