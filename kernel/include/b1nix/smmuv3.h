/*
 * ARM SMMUv3: the DMA remapping unit of an arm64 system-on-chip.
 *
 * The interface is deliberately the same shape as kernel/include/b1nix/iommu.h
 * (Intel VT-d) and amdvi.h (AMD-Vi) — a device is placed in a domain, pages are
 * granted to that domain, and anything the device touches that was not granted
 * faults — because that is what an IOMMU does regardless of whose architecture
 * it is. Nothing else is shared: the registers, the table formats and the
 * invalidation mechanism are all this architecture's own, and the markers the
 * self-test prints say SMMUv3 rather than borrowing VT-d's.
 */
#ifndef B1NIX_SMMUV3_H
#define B1NIX_SMMUV3_H

#include <b1nix/types.h>

/* Find the unit in the device tree and bring it up with every stream bypassing.
 * Silent and harmless on a board whose tree has no `arm,smmu-v3` node. */
void smmuv3_init(void);

/* Non-zero once the unit is enabled and translating. */
int smmuv3_active(void);

/* The StreamID a PCI function is seen as, from the host bridge's `iommu-map`.
 * Returns 0 if this requester does not go through the unit. */
int smmuv3_stream_id(u8 bus, u8 slot, u8 func, u32 *sid_out);

/* Move one PCI function into the translating domain, or back to bypass. */
int smmuv3_attach_device(u8 bus, u8 slot, u8 func);
void smmuv3_detach_device(u8 bus, u8 slot, u8 func);

/* Grant / revoke pages in the domain. Addresses and sizes are page-aligned. */
int smmuv3_map(u64 iova, u64 phys, usize size, int writable);
int smmuv3_unmap(u64 iova, usize size);

/* What the unit's own page tables say this device address resolves to, or 0. */
u64 smmuv3_translate(u64 iova);

/* Translation faults recorded in the event queue since the last clear. */
u32 smmuv3_fault_count(void);
void smmuv3_fault_clear(void);
/* The most recent fault: the address the device asked for, the StreamID that
 * asked, and the event type the unit recorded. */
void smmuv3_fault_last(u64 *addr, u32 *sid, u8 *type);

/* M100e: prove the above on real hardware. Runs only under b1nix.test=1. */
void smmuv3_selftest(void);

#endif /* B1NIX_SMMUV3_H */
