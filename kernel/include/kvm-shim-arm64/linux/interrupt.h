/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_INTERRUPT_H
#define KVM_SHIM_ARM64_LINUX_INTERRUPT_H
/*
 * Requesting host interrupts, arm64 KVM's part (M131): per-CPU interrupts
 * (PPIs) with a per-CPU cookie, enabled and disabled on the calling CPU, and
 * enable/disable of an interrupt a guest owns. linuxkpi's own enable_irq and
 * disable_irq do nothing, which is right for the x86 drivers that call them
 * and wrong here: KVM masks the host's copy of a forwarded interrupt while a
 * guest has it disabled. Implemented over b1nix's GIC in
 * kernel/lkpi/kvm_arm64_irq.c.
 */
#include_next <linux/interrupt.h>

#define IRQF_TRIGGER_NONE	0x00000000
#define IRQF_TRIGGER_RISING	0x00000001
#define IRQF_TRIGGER_FALLING	0x00000002
#define IRQF_TRIGGER_HIGH	0x00000004
#define IRQF_TRIGGER_LOW	0x00000008
#define IRQF_TRIGGER_MASK	(IRQF_TRIGGER_HIGH | IRQF_TRIGGER_LOW | \
				 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)

int request_percpu_irq(unsigned int irq, irq_handler_t handler,
		       const char *devname, void __percpu *percpu_dev_id);
void free_percpu_irq(unsigned int irq, void __percpu *percpu_dev_id);
void enable_percpu_irq(unsigned int irq, unsigned int type);
void disable_percpu_irq(unsigned int irq);

void lkpi_gic_enable_irq(unsigned int irq);
void lkpi_gic_disable_irq(unsigned int irq);
#define enable_irq(irq)		lkpi_gic_enable_irq(irq)
#define disable_irq_nosync(irq)	lkpi_gic_disable_irq(irq)
#endif
