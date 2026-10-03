/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_PGALLOC_H
#define KVM_SHIM_ARM64_ASM_PGALLOC_H
/* Host page-table allocation. KVM builds its stage-2 and hyp tables with its
 * own allocator (kvm_pgtable_mm_ops) and names nothing from here. */
#include <linux/mm.h>
#endif
