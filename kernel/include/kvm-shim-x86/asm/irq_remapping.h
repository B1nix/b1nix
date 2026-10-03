/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_IRQ_REMAPPING_H
#define KVM_SHIM_ASM_IRQ_REMAPPING_H
#include <linux/types.h>
/* Posting a device's interrupt straight into a vCPU needs VFIO and IOMMU
 * posted-interrupt support; neither is offered to KVM here. */
enum irq_remap_cap { IRQ_POSTING_CAP = 0 };
struct vcpu_data {
	u64 pi_desc_addr;
	u32 vector;
};
struct intel_iommu_pi_data {
	u64 pi_desc_addr;
	u32 vector;
	bool is_guest_mode;
};
static inline bool irq_remapping_cap(enum irq_remap_cap cap) { (void)cap; return false; }
static inline int irq_set_vcpu_affinity(unsigned int irq, void *vcpu_info)
{
	(void)irq; (void)vcpu_info;
	return -ENODEV;
}
#endif
