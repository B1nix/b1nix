/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_TYPES_H
#define KVM_SHIM_ARM64_LINUX_TYPES_H
/* Upstream's <linux/types.h> is empty in assembly; linuxkpi's is C only. */
#ifndef __ASSEMBLY__
#include_next <linux/types.h>
#endif
#endif
