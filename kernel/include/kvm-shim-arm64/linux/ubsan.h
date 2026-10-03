/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_UBSAN_H
#define KVM_SHIM_ARM64_LINUX_UBSAN_H
/* UBSAN is not built; a guest BRK carrying a UBSAN code is reported as a plain BRK. */
#include <linux/types.h>
static inline const char *report_ubsan_failure(u32 esr) { (void)esr; return NULL; }
#endif
