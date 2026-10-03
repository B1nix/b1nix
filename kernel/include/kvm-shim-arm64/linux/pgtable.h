/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_PGTABLE_H
#define KVM_SHIM_ARM64_LINUX_PGTABLE_H
/*
 * The page-table helpers, arm64's: linuxkpi's own <linux/pgtable.h> spells
 * x86 PTE bits and kernel protections, which mean something else here, so
 * this replaces it rather than extending it.
 */
#include <asm/pgtable.h>
#endif
