/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_PGTABLE_TYPES_H
#define KVM_SHIM_ARM64_ASM_PGTABLE_TYPES_H
/*
 * arm64 translation-table descriptors as typed values, laid out as upstream's
 * <asm/pgtable-types.h> with four levels (4 KiB pages, 48-bit VA). pgprot_t
 * is linuxkpi's (<linux/types.h>), the same single 64-bit word.
 */
#include <linux/types.h>

typedef u64 ptdesc_t;
typedef ptdesc_t pteval_t;
typedef ptdesc_t pmdval_t;
typedef ptdesc_t pudval_t;
typedef ptdesc_t p4dval_t;
typedef ptdesc_t pgdval_t;

typedef struct { pteval_t pte; } pte_t;
#define pte_val(x)	((x).pte)
#define __pte(x)	((pte_t) { (x) } )
typedef struct { pmdval_t pmd; } pmd_t;
#define pmd_val(x)	((x).pmd)
#define __pmd(x)	((pmd_t) { (x) } )
typedef struct { pudval_t pud; } pud_t;
#define pud_val(x)	((x).pud)
#define __pud(x)	((pud_t) { (x) } )
typedef struct { pgdval_t pgd; } pgd_t;
#define pgd_val(x)	((x).pgd)
#define __pgd(x)	((pgd_t) { (x) } )
#endif
