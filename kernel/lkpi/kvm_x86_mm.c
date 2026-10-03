// SPDX-License-Identifier: GPL-2.0-only
/*
 * A b1nix address space as x86 KVM walks it (M131): the page-table root it
 * sizes host mappings by, 5-level paging, and the memory-type questions
 * (PAT, the e820 map) VMX asks of a frame. The rest of the mm glue is
 * kvm_mm.c.
 */
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <asm/pgtable.h>
#include <asm/memtype.h>
#include <asm/e820/api.h>
#include <b1nix/kvm_bridge.h>

/* ── page tables ──────────────────────────────────────────────────────── */

/* KVM sizes its own mappings after the host's (host_pfn_mapping_level): a
 * guest range the process has mapped with a 2 MiB page (THP) goes into EPT or
 * NPT as one. With four-level paging an mm's top table is its PML4, which is
 * what the walk expects. Under LA57 b1nix keeps the process's PML4 below a
 * PML5 of its own, which the walk would have to start from; there it gets an
 * empty table and maps everything at 4 KiB -- slower, never wrong. The walk
 * runs with interrupts off, so a table freed after a shootdown cannot go away
 * under it. */
static pgd_t kvm_empty_pgd[512] __attribute__((aligned(4096)));

pgd_t *lkpi_mm_pgd(struct mm_struct *mm)
{
	if (!mm || !mm->pml4_phys || b1nix_kvm_la57())
		return kvm_empty_pgd;
	return (pgd_t *)(uintptr_t)(mm->pml4_phys + lkpi_direct_map_base());
}

int lkpi_paging_la57(void) { return b1nix_kvm_la57(); }

bool pat_pfn_immune_to_uc_mtrr(unsigned long pfn)
{
	u64 pa = (u64)pfn << PAGE_SHIFT;

	return b1nix_kvm_range_is_ram(pa, pa + PAGE_SIZE);
}

bool e820__mapped_raw_any(u64 start, u64 end, enum e820_type type)
{
	if (type != E820_TYPE_RAM)
		return false;
	return b1nix_kvm_range_is_ram(start, end);
}
