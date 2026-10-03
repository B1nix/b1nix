/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_MMU_CONTEXT_H
#define KVM_SHIM_ARM64_ASM_MMU_CONTEXT_H
/* Host address-space switching is b1nix's own, not Linux's. What KVM reaches
 * through this header upstream is the TLB, cache and Spectre-vector
 * vocabulary, imported unchanged. */
#include <linux/types.h>
#include <asm/cacheflush.h>
#include <asm/spectre.h>
#include <asm/tlbflush.h>
#endif
