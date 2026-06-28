#pragma once

/*
 * Generic device-IRQ registration (M70: interrupt-driven I/O).
 *
 * Block and other DMA drivers register a handler against a legacy PCI IRQ line
 * (the PCI config 0x3C "interrupt line", 0..15) instead of having their vector
 * hard-coded into the interrupt dispatcher. The x86 IRQ entry path looks the
 * line up (vector - 32) and calls every registered handler. PCI IRQ lines are
 * commonly shared, so a handler returns whether *its* device actually raised
 * the interrupt; the dispatcher ORs the results and only treats the IRQ as
 * spurious if no handler claimed it.
 *
 * Handlers run in interrupt context (IRQs off at the LAPIC level): they must be
 * short — ack the device, set a completion flag, and wake the blocked task via
 * scheduler_wake_all(). No sleeping, no blocking, no heap churn.
 */

#include <b1nix/types.h>

/* Returns 1 if this device raised the interrupt (and the handler serviced it),
 * 0 if the interrupt was not ours (shared-line case). */
typedef int (*irq_handler_fn)(void *ctx);

/* Register a handler for legacy IRQ line `irq` (0..15). Multiple devices may
 * share a line. Returns 0 on success, -1 if the line is out of range or its
 * sharer table is full. Does NOT unmask — call irq_unmask() once the device is
 * configured to raise interrupts. */
int irq_register_handler(u8 irq, irq_handler_fn fn, void *ctx);

/* Route and unmask the line at the IOAPIC (or legacy 8259). Thin wrapper over
 * the arch x86_pic_unmask so drivers don't reach into arch code directly. */
void irq_unmask(u8 irq);

/* Dispatch every handler registered for `irq` (called from the arch IRQ entry).
 * Returns the OR of the handlers' return values (1 if any claimed it). */
int irq_dispatch(int irq);
