/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_IRQBYPASS_H
#define LKPI_LINUX_IRQBYPASS_H
#include <linux/list.h>
/* Interrupt bypass (a passed-through device's interrupt posted straight to a
 * vCPU) needs VFIO, which this kernel does not have: the types exist for the
 * prototypes that name them, and nothing ever registers one. */
struct eventfd_ctx;
struct irq_bypass_consumer;

struct irq_bypass_producer {
	struct eventfd_ctx *eventfd;
	struct irq_bypass_consumer *consumer;
	int irq;
	int (*add_consumer)(struct irq_bypass_producer *, struct irq_bypass_consumer *);
	void (*del_consumer)(struct irq_bypass_producer *, struct irq_bypass_consumer *);
	void (*stop)(struct irq_bypass_producer *);
	void (*start)(struct irq_bypass_producer *);
};

struct irq_bypass_consumer {
	struct eventfd_ctx *eventfd;
	struct irq_bypass_producer *producer;
	int (*add_producer)(struct irq_bypass_consumer *, struct irq_bypass_producer *);
	void (*del_producer)(struct irq_bypass_consumer *, struct irq_bypass_producer *);
	void (*stop)(struct irq_bypass_consumer *);
	void (*start)(struct irq_bypass_consumer *);
};
#endif
