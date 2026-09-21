/* SPDX-License-Identifier: GPL-2.0-only */
/* Transparent huge pages for anonymous memory (M128).
 *
 * The policy half: whether the machine will use them at all, and whether a
 * particular mapping asked for them. The mechanism — installing a 2 MiB entry,
 * splitting one back into 512 leaves, and counting what a mapping holds — is
 * per-architecture and lives in the arch paging code (paging_thp_*).
 *
 * Off by default. `b1nix.thp` turns on madvise mode, `b1nix.thp=always` turns
 * it on for every eligible anonymous mapping. The state is readable and
 * writable at /sys/kernel/mm/transparent_hugepage/enabled, in Linux's format.
 */
#ifndef B1NIX_THP_H
#define B1NIX_THP_H

#include <b1nix/types.h>

struct vm_area;

#define THP_MODE_NEVER   0
#define THP_MODE_MADVISE 1
#define THP_MODE_ALWAYS  2

/* Read the boot command line. Called once, after bootinfo is available. */
void thp_init(void);

int thp_mode(void);
void thp_set_mode(int mode);

/* Would a 2 MiB entry be installed for this mapping? Answers the policy
 * question only: the caller still has to establish that the block is aligned,
 * wholly inside the mapping and empty. A NULL vma is never eligible — without
 * one there is no way to know the range is anonymous. */
int thp_vma_eligible(const struct vm_area *vma);

/* Counters behind /proc/vmstat-style reporting and the smoke test. */
void thp_count_alloc(void);
void thp_count_fallback(void);
void thp_count_split(void);
u64 thp_stat_alloc(void);
u64 thp_stat_fallback(void);
u64 thp_stat_split(void);

/* ── the arch mechanism, implemented in kernel/arch/<arch>/paging.c ────── */

/* Bytes of `[start, end)` in the address space at `pml4_phys` that are backed
 * by a transparent huge page. What /proc/<pid>/smaps reports as AnonHugePages,
 * and the only honest way to prove a mapping really is 2 MiB-backed. */
u64 paging_thp_bytes(u64 pml4_phys, u64 start, u64 end);

/* Break every transparent huge page overlapping [start, end) into its 512
 * ordinary leaves, so that a walker which knows nothing about huge entries is
 * correct again. Must be called with no page-table lock held: it allocates.
 *
 * `partial_only` splits just the blocks the range does not wholly cover — what
 * an unmap wants, since a block it covers completely can be released as a
 * block. Returns 0, or -ENOMEM when a table could not be allocated (in which
 * case nothing was changed for the block that failed).
 */
int paging_thp_split_range(u64 pml4_phys, u64 start, u64 end, int partial_only);

/* The same for every user mapping of an address space — what fork does before
 * it clones the page tables. */
int paging_thp_split_all(u64 pml4_phys);

#endif /* B1NIX_THP_H */
