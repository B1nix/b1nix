/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_INTERRUPT_H
#define LKPI_LINUX_INTERRUPT_H
#include <linux/irqreturn.h>
#include <linux/types.h>
/* Handler registration is not wired up yet: b1nix installs interrupt handlers
 * through its own IDT/IOAPIC paths, and which of those a DRM driver should use
 * is M102's question, not the core's. Declared so the core compiles; the first
 * driver that needs a real request_irq gets one then. */
typedef irqreturn_t (*irq_handler_t)(int, void *);
#define IRQF_SHARED 0x00000080
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
                const char *name, void *dev);
void free_irq(unsigned int irq, void *dev);
static inline void disable_irq(unsigned int irq) { (void)irq; }
static inline void enable_irq(unsigned int irq) { (void)irq; }
#endif
