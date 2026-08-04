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

/* Move one PCI function out of pass-through into the translated domain, so
 * only what iommu_map() installs is reachable from it. Returns 0 on success. */
int iommu_attach_device(u8 bus, u8 slot, u8 func);

/* Put it back into pass-through. */
void iommu_detach_device(u8 bus, u8 slot, u8 func);

/* Install (or remove) a mapping in the translated domain. `iova` and `phys`
 * must be page aligned; `size` is rounded up. `writable` allows device writes.
 * Returns 0 on success. */
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

/* Faults the unit has recorded (a device touching an address nobody gave it),
 * and a reset of that state. Reading clears the records. */
u32 iommu_fault_count(void);
void iommu_fault_clear(void);

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
