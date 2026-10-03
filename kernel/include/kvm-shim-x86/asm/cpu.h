/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_CPU_H
#define KVM_SHIM_ASM_CPU_H
#include <asm/processor.h>
/* Split-lock detection is not configured: a split lock in a guest is the
 * guest's business, and #AC is injected rather than handled by the host. */
static inline bool handle_guest_split_lock(unsigned long ip) { (void)ip; return false; }

/* Intel family-6 models KVM names in its quirk tables (asm/intel-family.h). */
#ifndef INTEL_NEHALEM
#define IFM(fam, model) (((X86_VENDOR_INTEL) << 16) | ((fam) << 8) | (model))
#define INTEL_NEHALEM          IFM(6, 0x1E)
#define INTEL_NEHALEM_EP       IFM(6, 0x1A)
#define INTEL_NEHALEM_EX       IFM(6, 0x2E)
#define INTEL_WESTMERE         IFM(6, 0x25)
#define INTEL_WESTMERE_EP      IFM(6, 0x2C)
#define INTEL_ICELAKE_X        IFM(6, 0x6A)
#define INTEL_ICELAKE_D        IFM(6, 0x6C)
#define INTEL_SAPPHIRERAPIDS_X IFM(6, 0x8F)
#endif

#endif
