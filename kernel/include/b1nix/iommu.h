#ifndef B1NIX_IOMMU_H
#define B1NIX_IOMMU_H

#include <b1nix/types.h>

/*
 * M100b: Intel VT-d DMA remapping.
 *
 * Without translation a device address is a physical address, so a device can
 * reach every byte of memory whatever it was handed, and a device whose window
 * is too narrow has to be served by copying (M99/M100a bounce buffers). VT-d
 * gives each device its own address space: the driver asks for a mapping, the
 * device sees an address that exists only through the unit's page tables, and
 * anything else it emits faults.
 *
 * Every device starts in pass-through, so enabling the unit changes nothing for
 * a driver that has not asked for translation. A driver opts in with
 * iommu_attach_device(), after which iommu_map()/iommu_unmap() decide what that
 * device can reach.
 */

/* Parse the ACPI DMAR table, program the root/context tables (pass-through for
 * everything) and enable translation. No-op when there is no DMAR. */
void iommu_init(void);

/* 1 once translation is enabled on at least one remapping unit. */
int iommu_active(void);

/* Width of the device address space, in bits (0 when inactive). */
u32 iommu_address_width(void);

/*
 * Domains and groups (M100c).
 *
 * A domain is one device address space: its own page tables, its own id. Giving
 * each device its own is the difference between "devices cannot reach the
 * kernel's memory" and "devices cannot reach each other's either".
 *
 * A group is the set of functions that cannot be isolated from one another —
 * the functions of a multifunction device share one context in practice, so
 * they share a domain and move together. Attaching one function attaches its
 * group; there is no way to ask for less.
 */
struct iommu_domain;

/* Create an empty domain (no memory reachable from it). NULL when the unit is
 * inactive or out of domain ids. */
struct iommu_domain *iommu_domain_create(void);
void iommu_domain_destroy(struct iommu_domain *dom);
u16 iommu_domain_id(const struct iommu_domain *dom);

/* Highest domain id the unit can tell apart (from its own capability
 * register), 0 when inactive. */
u16 iommu_domain_capacity(void);

/* Move a function — and every other function in its group — into `dom`.
 * Passing NULL puts the group back where it started. */
int iommu_attach_group(struct iommu_domain *dom, u8 bus, u8 slot, u8 func);

/* The group a function belongs to, as a stable number. Two functions with the
 * same group number cannot be isolated from each other. */
u32 iommu_group_of(u8 bus, u8 slot, u8 func);

/* Move one PCI function into the default translated domain (the M100b
 * behaviour), or back out of it. */
int iommu_attach_device(u8 bus, u8 slot, u8 func);
void iommu_detach_device(u8 bus, u8 slot, u8 func);

/* Install (or remove) a mapping in a specific domain. The unqualified forms
 * act on the default translated domain. */
int iommu_domain_map(struct iommu_domain *dom, u64 iova, u64 phys, usize size,
                     int writable);
int iommu_domain_unmap(struct iommu_domain *dom, u64 iova, usize size);
u64 iommu_domain_translate(const struct iommu_domain *dom, u64 iova);

int iommu_map(u64 iova, u64 phys, usize size, int writable);
int iommu_unmap(u64 iova, usize size);

/* Physical address a device address currently translates to, 0 when unmapped.
 * Reads the page tables the hardware walks, so a test can check the mapping
 * rather than the request. */
u64 iommu_translate(u64 iova);

/* Map a physical range at the same device address. A driver that already
 * programs its own physical addresses keeps working once its pages are mapped
 * this way — and reaches nothing else, which is the point. */
int iommu_map_identity(u64 phys, usize size, int writable);

/*
 * Interrupt remapping (M100c).
 *
 * With it on, an MSI is no longer a raw vector written to an address: the
 * message names an entry in a table the kernel owns, and the unit reads the
 * destination and vector from there. A device cannot invent an interrupt it was
 * not given an entry for.
 */
int iommu_ir_active(void);

/* Program an entry for (vector, destination APIC id) and return its handle, or
 * -1. `source` is the requester id (bus<<8 | devfn) the entry is bound to, so
 * an interrupt claiming that handle from anywhere else is rejected. */
int iommu_ir_alloc(u8 vector, u32 apic_id, u16 source);
void iommu_ir_free(int handle);

/* The address/data pair a device must be programmed with to use `handle`. */
u64 iommu_ir_message_address(int handle);
u32 iommu_ir_message_data(int handle);

/* Contents of an entry, for a test that wants to check what was programmed
 * rather than what was asked for. Returns 0 when the entry is present. */
int iommu_ir_entry_read(int handle, u8 *vector, u32 *apic_id, u16 *source);

/* Faults the unit has recorded (a device touching an address nobody gave it),
 * and a reset of that state. Reading clears the records. */
u32 iommu_fault_count(void);
void iommu_fault_clear(void);

/* Details of the most recent fault: the address the device asked for, the
 * requester id that asked, and the unit's reason code. */
void iommu_fault_last(u64 *addr, u16 *source, u8 *reason);

/* Allocate/free a device address range from the IOMMU's own address space. */
u64 iommu_iova_alloc(usize size);
void iommu_iova_free(u64 iova, usize size);

/* M100b: give the AC'97 codec its descriptor list and nothing else, start
 * playback, and see whether the unit blocks and records the audio-buffer read
 * nobody granted. 1 = fault recorded, 0 = not, -1 = no codec. Implemented in
 * kernel/dev/ac97.c, where the device's internals live. */
int ac97_iommu_violation_probe(void);

/* M100b self-test: DMAR parsing, translation enable, map/unmap round-trip and a
 * real device driving DMA through the unit. Emits M100B-SMOKE markers. */
void iommu_selftest(void);

#endif
