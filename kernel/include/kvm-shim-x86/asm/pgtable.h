/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_PGTABLE_H
#define KVM_SHIM_ASM_PGTABLE_H
/*
 * Read-only walks of a b1nix address space's page tables, in Linux's
 * vocabulary (M131). KVM uses them to find how large a host mapping is
 * (host_pfn_mapping_level) so it can map the guest with the same page size.
 *
 * b1nix's tables are ordinary x86 tables reachable through the direct map,
 * four levels or five (CR4.LA57, decided at boot): with four, the p4d level
 * is folded into the pgd as on Linux.
 */
#include <linux/types.h>
#include <asm/pgtable_types.h>

typedef struct { u64 pgd; } pgd_t;
typedef struct { u64 p4d; } p4d_t;
typedef struct { u64 pud; } pud_t;
typedef struct { u64 pmd; } pmd_t;

#define pgd_val(x) ((x).pgd)
#define p4d_val(x) ((x).p4d)
#define pud_val(x) ((x).pud)
#define pmd_val(x) ((x).pmd)

#define _PAGE_BIT_PSE 7
#define _PAGE_PSE     (1ULL << _PAGE_BIT_PSE)
#define PTE_PFN_MASK  0x000ffffffffff000ULL

int lkpi_paging_la57(void);
#define pgtable_l5_enabled() lkpi_paging_la57()

#define PTRS_PER_PTE 512
#define PTRS_PER_PMD 512
#define PTRS_PER_PUD 512
#define PTRS_PER_P4D (pgtable_l5_enabled() ? 512 : 1)
#define P4D_SHIFT 39
#define PGDIR_SHIFT_L (pgtable_l5_enabled() ? 48 : 39)
#define PMD_SIZE (1UL << 21)
#define PUD_SIZE (1UL << 30)
#define PMD_MASK (~(PMD_SIZE - 1))
#define PUD_MASK (~(PUD_SIZE - 1))

static inline void *lkpi_pt_va(u64 entry)
{
	extern unsigned long lkpi_direct_map_base(void);
	return (void *)((entry & PTE_PFN_MASK) + lkpi_direct_map_base());
}

struct mm_struct;
pgd_t *lkpi_mm_pgd(struct mm_struct *mm);

static inline pgd_t *pgd_offset(struct mm_struct *mm, unsigned long a)
{
	return lkpi_mm_pgd(mm) + ((a >> PGDIR_SHIFT_L) & 511);
}
static inline p4d_t *p4d_offset(pgd_t *pgd, unsigned long a)
{
	if (!pgtable_l5_enabled())
		return (p4d_t *)pgd;
	return (p4d_t *)lkpi_pt_va(pgd_val(*pgd)) + ((a >> P4D_SHIFT) & 511);
}
static inline pud_t *pud_offset(p4d_t *p4d, unsigned long a)
{
	return (pud_t *)lkpi_pt_va(p4d_val(*p4d)) + ((a >> 30) & 511);
}
static inline pmd_t *pmd_offset(pud_t *pud, unsigned long a)
{
	return (pmd_t *)lkpi_pt_va(pud_val(*pud)) + ((a >> 21) & 511);
}

#define pgd_none(x)    (!(pgd_val(x) & 1))
#define pgd_present(x) (pgd_val(x) & 1)
#define p4d_none(x)    (!(p4d_val(x) & 1))
#define p4d_present(x) (p4d_val(x) & 1)
#define pud_none(x)    (!(pud_val(x) & 1))
#define pud_present(x) (pud_val(x) & 1)
#define pmd_none(x)    (!(pmd_val(x) & 1))
#define pmd_present(x) (pmd_val(x) & 1)
#define pud_leaf(x)    (pud_present(x) && (pud_val(x) & _PAGE_PSE))
#define pmd_leaf(x)    (pmd_present(x) && (pmd_val(x) & _PAGE_PSE))
#define p4d_leaf(x)    0
#define pgd_leaf(x)    0
#define pmdp_get_lockless(p) (*(volatile pmd_t *)(p))
#endif
