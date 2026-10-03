/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_COMPILER_TYPES_H
#define KVM_SHIM_ARM64_LINUX_COMPILER_TYPES_H
/* As on Linux, the architecture's compiler definitions come with the generic
 * ones: ARM64_ASM_PREAMBLE for the TLBI and system-register asm. */
#include_next <linux/compiler_types.h>
#include <asm/compiler.h>
#endif
