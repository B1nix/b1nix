/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_IRQDOMAIN_H
#define KVM_SHIM_ARM64_LINUX_IRQDOMAIN_H
/*
 * Interrupt domains, as arm64 KVM stacks one over the GIC's. It does so in
 * two cases only: a host GIC that cannot deactivate a forwarded interrupt
 * from a guest (the timer then gets a domain of its own that masks instead),
 * and GICv4 direct injection. b1nix's GIC driver (QEMU virt, GICv3) supports
 * hardware deactivation and has no GICv4, so the GIC domain is flat -- one
 * interrupt per INTID -- and creating a hierarchy over it fails, which KVM
 * reports as the missing feature it is.
 */
#include <linux/types.h>

struct irq_chip;
struct irq_data;
struct irq_domain;
struct fwnode_handle;

struct irq_domain_ops {
	int (*alloc)(struct irq_domain *d, unsigned int virq,
		     unsigned int nr_irqs, void *arg);
	void (*free)(struct irq_domain *d, unsigned int virq,
		     unsigned int nr_irqs);
};

struct irq_domain {
	const char			*name;
	const struct irq_domain_ops	*ops;
	struct irq_domain		*parent;
	struct fwnode_handle		*fwnode;
	void				*host_data;
};

struct fwnode_handle *irq_domain_alloc_named_fwnode(const char *name);
void irq_domain_free_fwnode(struct fwnode_handle *fwnode);
struct irq_domain *irq_domain_create_hierarchy(struct irq_domain *parent,
					       unsigned int flags,
					       unsigned int size,
					       struct fwnode_handle *fwnode,
					       const struct irq_domain_ops *ops,
					       void *host_data);
int irq_domain_push_irq(struct irq_domain *domain, int virq, void *arg);
int irq_domain_set_hwirq_and_chip(struct irq_domain *domain,
				  unsigned int virq, irq_hw_number_t hwirq,
				  const struct irq_chip *chip, void *chip_data);
unsigned int irq_find_mapping(struct irq_domain *domain,
			      irq_hw_number_t hwirq);
int irq_domain_activate_irq(struct irq_data *d, bool reserve);
void irq_domain_deactivate_irq(struct irq_data *d);
#endif
