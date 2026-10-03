/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_IRQ_H
#define KVM_SHIM_ARM64_LINUX_IRQ_H
/*
 * Host interrupts as arm64 KVM drives them (M131): the per-CPU timer and
 * vGIC maintenance interrupts it requests, and the GIC state of an interrupt
 * it forwards to a guest (the virtual timer, whose active state a guest owns
 * while it runs). Linux numbers these through irq domains; here the Linux
 * number of a GIC interrupt is its INTID, and kernel/lkpi/kvm_arm64_irq.c
 * keeps one descriptor per INTID on top of b1nix's GIC driver.
 */
#include_next <linux/irq.h>
#include <linux/cpumask.h>
#include <linux/irqdomain.h>

/* Descriptor state bits (irqd_*). */
#define IRQD_IRQ_DISABLED		(1u << 16)
#define IRQD_FORWARDED_TO_VCPU		(1u << 17)

/* Status flags a caller sets on an interrupt (irq_set_status_flags). */
#define IRQ_NOAUTOEN			(1u << 12)
#define IRQ_NO_BALANCING		(1u << 13)
#define IRQ_DISABLE_UNLAZY		(1u << 19)

struct irq_data {
	unsigned int		irq;
	irq_hw_number_t		hwirq;
	unsigned int		state;
	struct irq_chip		*chip;
	struct irq_domain	*domain;
	struct irq_data		*parent_data;
	void			*chip_data;
};

struct irq_desc {
	struct irq_data		irq_data;
	irq_handler_t		handler;
	void __percpu		*percpu_dev_id;
	const char		*name;
	unsigned int		status;
	unsigned int		trigger;
};

struct irq_desc *irq_to_desc(unsigned int irq);
struct irq_data *irq_get_irq_data(unsigned int irq);

static inline struct irq_data *irq_desc_get_irq_data(struct irq_desc *desc)
{
	return &desc->irq_data;
}

static inline bool irqd_irq_disabled(struct irq_data *d)
{
	return d->state & IRQD_IRQ_DISABLED;
}

static inline bool irqd_is_forwarded_to_vcpu(struct irq_data *d)
{
	return d->state & IRQD_FORWARDED_TO_VCPU;
}

static inline void irqd_set_forwarded_to_vcpu(struct irq_data *d)
{
	d->state |= IRQD_FORWARDED_TO_VCPU;
}

static inline void irqd_clr_forwarded_to_vcpu(struct irq_data *d)
{
	d->state &= ~IRQD_FORWARDED_TO_VCPU;
}

u32 irq_get_trigger_type(unsigned int irq);
void irq_set_status_flags(unsigned int irq, unsigned long set);
void irq_clear_status_flags(unsigned int irq, unsigned long clr);

int irq_set_irqchip_state(unsigned int irq, enum irqchip_irq_state which,
			  bool val);
int irq_get_irqchip_state(unsigned int irq, enum irqchip_irq_state which,
			  bool *state);
int irq_set_vcpu_affinity(unsigned int irq, void *vcpu_info);
int irq_set_affinity(unsigned int irq, const struct cpumask *cpumask);

/* A hierarchical chip's operations forwarded to the parent domain's chip. */
void irq_chip_mask_parent(struct irq_data *data);
void irq_chip_unmask_parent(struct irq_data *data);
void irq_chip_eoi_parent(struct irq_data *data);
int irq_chip_set_type_parent(struct irq_data *data, unsigned int type);
int irq_chip_set_parent_state(struct irq_data *data,
			      enum irqchip_irq_state which, bool val);
#endif
