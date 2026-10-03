/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_MM_TYPES_H
#define KVM_SHIM_ARM64_LINUX_MM_TYPES_H
/* The address-space types, plus the architecture's deferred-TLB-flush batch
 * Linux declares with them (<linux/mm_types_task.h>). */
#include_next <linux/mm_types.h>
#include <asm/tlbbatch.h>
#endif
