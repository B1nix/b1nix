/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_MEMTYPE_H
#define KVM_SHIM_ASM_MEMTYPE_H
#include <linux/types.h>
/* Whether a PFN is RAM whose PAT type makes an MTRR UC setting irrelevant
 * (Linux's memtype tracker). b1nix tracks no memtypes; ordinary RAM is
 * always write-back to it, so the question is whether the PFN is RAM. */
bool pat_pfn_immune_to_uc_mtrr(unsigned long pfn);
#endif
