/* SPDX-License-Identifier: MIT */
#ifndef LKPI_ASM_TSC_H
#define LKPI_ASM_TSC_H
#include <linux/types.h>
/* The raw cycle counter. Used for short spin timing, where the tick-derived
 * clock in <linux/sched/clock.h> is far too coarse. */
static inline u64 rdtsc(void)
{
	u32 lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

/* The TSC's frequency in kHz, as b1nix calibrated it at boot. Zero would mean
 * uncalibrated; the callers here divide by it, so they check first. */
extern unsigned int tsc_khz;

#endif
