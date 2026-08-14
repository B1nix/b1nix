/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PROCESSOR_H
#define LKPI_LINUX_PROCESSOR_H
#include <asm/cpufeature.h>
#include <linux/types.h>

/*
 * What a driver reads off the boot CPU: family, model and stepping to select a
 * workaround, plus the feature tests in <asm/cpufeature.h>.
 *
 * Filled from CPUID rather than left zeroed. A zeroed struct silently reads as
 * "family 0", which no quirk table expects — so every model-keyed workaround
 * would either all match or all miss, and which of the two is worse depends on
 * the quirk.
 */
struct cpuinfo_x86 {
	u8 x86;              /* family */
	u8 x86_model;
	u8 x86_stepping;
	u32 x86_capability[4];
	/* CLFLUSH line size in bytes, from CPUID leaf 1 EBX[15:8] scaled by 8.
	 * Real: the flush loops step by it. */
	unsigned int x86_clflush_size;
};

extern struct cpuinfo_x86 boot_cpu_data;

/* Filled once during init, before any driver probes. */
void lkpi_cpuinfo_init(void);
#endif
