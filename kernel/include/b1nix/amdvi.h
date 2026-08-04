#ifndef B1NIX_AMDVI_H
#define B1NIX_AMDVI_H

#include <b1nix/types.h>

/*
 * AMD-Vi (AMD I/O Virtualization Technology).
 *
 * Same job as VT-d and a different shape for every part of it. The unit is
 * described by ACPI IVRS rather than DMAR; devices are looked up in one flat
 * device table indexed by the whole requester id rather than a root table of
 * per-bus context tables; page tables are its own format, with the number of
 * levels carried in each entry rather than fixed per domain; and invalidation
 * goes through a command ring in memory instead of a register write.
 *
 * The one thing kept identical is the policy: every device starts able to
 * reach what it always could, and translation applies to a device only once
 * its driver asks. Anything else would fault every driver in the tree the
 * moment the unit came up.
 */

/* Parse IVRS, program the device table and enable translation. No-op when the
 * machine has no AMD-Vi. */
void amdvi_init(void);

/* 1 once the unit is translating. */
int amdvi_active(void);

/* Map/unmap in the domain a device was given. Addresses are page aligned. */
int amdvi_map(u64 iova, u64 phys, usize size, int writable);
int amdvi_unmap(u64 iova, usize size);

/* What a device address resolves to, read out of the tables the unit walks. */
u64 amdvi_translate(u64 iova);

/* Move a function into the translated domain, or back to passthrough. */
int amdvi_attach_device(u16 bdf);
void amdvi_detach_device(u16 bdf);

/* Faults the unit has logged, and a reset of that log. */
u32 amdvi_fault_count(void);
void amdvi_fault_clear(void);

/* M100d self-test. Emits M100D-SMOKE markers; no-op outside b1nix.test=1. */
void amdvi_selftest(void);

#endif
