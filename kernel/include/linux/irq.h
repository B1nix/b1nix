/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_IRQ_H
#define LKPI_LINUX_IRQ_H
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
/* The interrupt *handling* interface a driver uses — request_irq and friends —
 * is in <linux/interrupt.h> and is real. This header is the irq-chip side:
 * describing and routing lines, which is the platform's job and b1nix's own PIC
 * and IOAPIC code does it. Nothing imported drives an irq chip here. */

/* Allocating and freeing interrupt descriptors. b1nix's vectors are assigned by
 * its own IOAPIC/MSI code before a driver is bound, so there is no descriptor
 * pool to draw from here — the allocation reports failure and the free has
 * nothing to release. A driver that needs its own vector will see the failure
 * rather than a number that maps to nothing. */
static inline int irq_alloc_descs(int irq, unsigned int from, unsigned int cnt,
                                  int node)
{ (void)irq; (void)from; (void)cnt; (void)node; return -ENOSYS; }
static inline void irq_free_desc(unsigned int irq) { (void)irq; }
static inline void irq_free_descs(unsigned int irq, unsigned int cnt)
{ (void)irq; (void)cnt; }


/* An interrupt controller, as the core drives it. b1nix owns its IOAPIC and
 * MSI paths and nothing imported installs a chip, so this is the shape only —
 * a driver that registered one would find nothing calling it. */
struct irq_data;
struct irq_chip {
	const char *name;
	void (*irq_mask)(struct irq_data *data);
	void (*irq_unmask)(struct irq_data *data);
	void (*irq_ack)(struct irq_data *data);
	void (*irq_eoi)(struct irq_data *data);
};


/*
 * Building an IRQ domain for a device that demultiplexes its own interrupts.
 *
 * Declared and deliberately not defined. b1nix's interrupt layer maps vectors
 * to handlers at the APIC level and has no chained-chip abstraction: there is
 * no irq_desc to allocate and no chip to attach. i915's GSC sub-device is the
 * only caller; it must fail to link rather than register a chip that never
 * receives anything.
 */
struct irq_chip;
struct irq_desc;
int irq_alloc_desc(int node);
void irq_free_desc(unsigned int irq);
int irq_set_chip_and_handler_name(unsigned int irq, const struct irq_chip *chip,
                                  void (*handle)(struct irq_desc *desc),
                                  const char *name);
int irq_set_chip_data(unsigned int irq, void *data);
void *irq_get_chip_data(unsigned int irq);
void handle_simple_irq(struct irq_desc *desc);
int generic_handle_irq(unsigned int irq);

#endif
