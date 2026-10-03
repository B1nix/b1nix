/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_SMP_PLAT_H
#define KVM_SHIM_ARM64_ASM_SMP_PLAT_H
/* A logical CPU's MPIDR affinity, from b1nix's CPU bring-up. */
#include <linux/types.h>
u64 cpu_logical_map(unsigned int cpu);
#endif
