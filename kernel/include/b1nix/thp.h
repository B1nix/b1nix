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
struct task;

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
void thp_count_collapse(void);
u64 thp_stat_alloc(void);
u64 thp_stat_fallback(void);
u64 thp_stat_split(void);
u64 thp_stat_collapse(void);

/* ── khugepaged ───────────────────────────────────────────────────────────
 *
 * The fault path can only take a block where nothing is mapped yet, so a
 * program that faulted its arena in before anything asked for huge pages never
 * gets one however long it runs — which is most long-lived programs. This is
 * the other direction: a background thread that finds a 2 MiB range already
 * backed by 512 ordinary pages and replaces it with one block.
 *
 * Started when the feature is on (at boot, or when the sysfs knob turns it on),
 * and asleep the rest of the time. `scan_sleep_millisecs` is the pause between
 * passes, as in Linux, and is writable through
 * /sys/kernel/mm/transparent_hugepage/khugepaged/. */
void thp_khugepaged_maybe_start(void);
u64 thp_scan_sleep_ms(void);
void thp_set_scan_sleep_ms(u64 ms);

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

/* Break up to `max_blocks` of one task's blocks so that reclaim can reach the
 * pages inside them, and register the leaves to that task. Returns how many
 * blocks were really split.
 *
 * A block is not reclaimable while it is a block: the eviction ring holds
 * 4 KiB pages and there is nothing in it to take one page of a 2 MiB entry.
 * So something has to split first, and it cannot be reclaim itself — reclaim
 * is reached from inside the allocator, splitting allocates, and the two
 * orders do not agree (it wedged the machine, twice). This is called one level
 * out, where the charge against memory.max is decided and before the
 * allocation that would fail, with no lock held and in task context. */
int paging_thp_split_for_reclaim(struct task *task, usize max_blocks);

/* Replace the 512 pages covering `base` in this task's address space with one
 * block, copying their contents into it. Refused unless every one of them is a
 * present, private, anonymous page with a refcount of one and the same
 * permissions as the others — see the implementation for why each of those has
 * to hold. Returns 1 when a block was installed. What khugepaged calls. */
int paging_thp_collapse(struct task *task, struct vm_area *vma, u64 base);

/* The page table a block replaced, kept until its address space dies.
 *
 * A 2 MiB entry can only be installed where the directory entry names no
 * table — otherwise the table would have to be freed, and the walkers that
 * descend without the page-table lock (vmm_set_lazy, the /proc walkers) may
 * still hold a pointer into it; the allocator would have made the frame
 * something else by the time they read it. An address range that has already
 * been faulted at 4 KiB keeps its table when the mapping goes, so without
 * this every recycled range is served 4 KiB at a time for ever.
 *
 * So an EMPTY table is taken out of the tree and remembered here instead: the
 * frame stays claimed as a page table, a walker mid-descent reads the same
 * zeroes it read before, and the frame goes back at teardown, when nothing can
 * be walking it. The two words the list writes into the frame are page-aligned
 * physical addresses, so a stale walker reads them as absent entries.
 *
 * The cost is one frame per recycled 2 MiB range for the life of the process:
 * a fiftieth of a percent of the range it covers. */
void thp_orphan_table(u64 frame, u64 owner_space);
void thp_release_orphan_tables(u64 owner_space);

/* The kernel-readable address of a page-table frame, or NULL when the frame is
 * not reachable. Implemented per architecture beside the walkers that use it:
 * the direct map on x86_64, the identity map on aarch64. */
u64 *paging_table_map(u64 frame);

#endif /* B1NIX_THP_H */
