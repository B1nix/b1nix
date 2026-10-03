/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_TIMECOUNTER_H
#define KVM_SHIM_ARM64_LINUX_TIMECOUNTER_H
/* The cyclecounter/timecounter pair the arch timer exports to KVM: the
 * counter's frequency as a mult/shift, which KVM uses to turn a guest timer's
 * remaining cycles into nanoseconds. */
#include <linux/types.h>
struct cyclecounter { u64 (*read)(const struct cyclecounter *cc); u64 mask; u32 mult; u32 shift; };
struct timecounter { const struct cyclecounter *cc; u64 cycle_last; u64 nsec; u64 mask; u64 frac; };

static inline u64 cyclecounter_cyc2ns(const struct cyclecounter *cc,
				      u64 cycles, u64 mask, u64 *frac)
{
	u64 ns = (u64)cycles;

	ns = (ns * cc->mult) + *frac;
	*frac = ns & mask;
	return ns >> cc->shift;
}
#endif
