/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_HW_BREAKPOINT_H
#define KVM_SHIM_ARM64_LINUX_HW_BREAKPOINT_H
/* No perf hardware breakpoints in the host: the debug registers are KVM's to
 * save and restore. It sizes a guest's by the CPU's counts. */
#include <asm/cpufeature.h>
#include <asm/sysreg.h>

static inline int get_num_brps(void)
{
	u64 dfr0 = read_sanitised_ftr_reg(SYS_ID_AA64DFR0_EL1);

	return 1 + cpuid_feature_extract_unsigned_field(dfr0,
						ID_AA64DFR0_EL1_BRPs_SHIFT);
}

static inline int get_num_wrps(void)
{
	u64 dfr0 = read_sanitised_ftr_reg(SYS_ID_AA64DFR0_EL1);

	return 1 + cpuid_feature_extract_unsigned_field(dfr0,
						ID_AA64DFR0_EL1_WRPs_SHIFT);
}
#endif
