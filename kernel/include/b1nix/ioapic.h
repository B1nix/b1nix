#ifndef B1NIX_IOAPIC_H
#define B1NIX_IOAPIC_H

#include <b1nix/types.h>

/*
 * IOAPIC IRQ routing (A1 follow-on: replaces the 8259 PIC for the IRQs we
 * actually use). Uses the IOAPIC address + Interrupt Source Override list
 * parsed by acpi_init().
 *
 * Lifecycle:
 *   acpi_init()        — parse MADT (already wired in main.c)
 *   lapic_init()       — set up LAPIC + EOI
 *   ioapic_init()      — map MMIO, mask the 8259, install routes for IRQ 0/1/12
 *   ioapic_route_irq() — programmed on-demand (e.g. NIC) once the device
 *                        knows its IRQ number
 *
 * After ioapic_init returns 1, all IRQ-path EOIs MUST go through
 * lapic_eoi() (handled centrally by interrupts.c via ioapic_active()).
 */

/* Returns 1 if an IOAPIC was found, mapped, and IRQ 0/1/12 routed; 0 if no
 * IOAPIC exists (legacy PIC mode stays in effect) or mapping failed. */
int  ioapic_init(void);

/* True if ioapic_init succeeded and we're driving IRQs through the IOAPIC. */
int  ioapic_active(void);

/* Program a redirection entry for a legacy ISA IRQ:
 *   legacy_irq — what the device thinks its IRQ line is (0..15).
 *   vector     — CPU IDT vector to fire (typically 32 + legacy_irq).
 *   dest_apic  — destination LAPIC ID (usually the BSP).
 *   level_low  — 1 for PCI-style (level-triggered, active-low), 0 for ISA
 *                defaults; any ACPI ISO entry overrides both.
 * After the call the IRQ is unmasked. */
void ioapic_route_irq(u8 legacy_irq, u8 vector, u8 dest_apic, int level_low);

/* Mask/unmask an already-routed IRQ. */
void ioapic_mask_irq(u8 legacy_irq);
void ioapic_unmask_irq(u8 legacy_irq);

#endif /* B1NIX_IOAPIC_H */
