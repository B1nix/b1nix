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

/* Remove a (fn, ctx) handler previously registered on `irq`. Returns 0 if found
 * and cleared, -1 otherwise. Safe against a concurrent dispatcher. */
int irq_unregister_handler(u8 irq, irq_handler_fn fn, void *ctx);

/* Route and unmask the line at the IOAPIC (or legacy 8259). Thin wrapper over
 * the arch x86_pic_unmask so drivers don't reach into arch code directly. */
void irq_unmask(u8 irq);

/* Dispatch every handler registered for `irq` (called from the arch IRQ entry).
 * Returns the OR of the handlers' return values (1 if any claimed it). */
int irq_dispatch(int irq);

/*
 * M98: message-signalled interrupts (MSI / MSI-X).
 *
 * An MSI has no line. The device writes the vector straight to the local APIC,
 * so there is nothing to route at the IOAPIC, nothing to mask there, and no
 * line shared with a legacy device — which is exactly why these do not belong
 * in the 16-entry table above. Vectors MSI_VECTOR_BASE..+MSI_VECTOR_COUNT-1
 * have their own IDT gates and one owner each.
 *
 * A driver asks for a vector, programs it into the device's MSI or MSI-X
 * capability (pci_msi_enable / pci_msix_enable), and its handler is called with
 * the registered ctx when the device raises it. Unlike a shared line the return
 * value is not a claim — the vector belongs to one device — but it is kept the
 * same shape as irq_handler_fn so a driver can use one handler for both paths.
 */
#define MSI_VECTOR_BASE  48u
#define MSI_VECTOR_COUNT 16u

/* Claim a free MSI vector for (fn, ctx). Returns the vector number (48..63) to
 * program into the device, or -1 when every vector is taken. */
int msi_alloc_vector(irq_handler_fn fn, void *ctx);

/* Release a vector claimed by msi_alloc_vector. The caller must have disabled
 * the device's MSI/MSI-X capability first. */
void msi_free_vector(int vector);

/* Call the owner of `vector` (arch IRQ entry). Returns 1 if a handler ran. */
int msi_dispatch(int vector);

/* Build the address/data pair a device must write to raise `vector`, and do
 * whatever the interrupt controller needs first (on aarch64 that is the ITS
 * mapping commands). Called from pci_msi_enable/pci_msix_enable. */
int arch_msi_prepare(u8 bus, u8 slot, u8 func, int vector, u64 *addr_out,
                     u32 *data_out);

/* Can this machine deliver a message-signalled interrupt at all? x86 always
 * can; aarch64 needs a GICv3 with an ITS. */
int arch_msi_supported(void);

/* What arch_msi_prepare programmed for `vector`, for a self-test that checks a
 * device is armed for the interrupt it thinks it is. */
int arch_msi_expected(int vector, u64 *addr_out, u32 *data_out);
