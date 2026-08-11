/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_PGTABLE_H
#define LKPI_LINUX_PGTABLE_H
#include <linux/mm.h>

/* Page-table attribute bits, for code that builds a PTE value directly. */
#define _PAGE_PRESENT  0x001
#define _PAGE_RW       0x002
#define _PAGE_USER     0x004
#define _PAGE_PWT      0x008
#define _PAGE_PCD      0x010
#define _PAGE_ACCESSED 0x020
#define _PAGE_DIRTY    0x040
#define _PAGE_PAT      0x080


/* A page-table entry, as a value. Code that builds one directly names the type;
 * b1nix's own paging works in plain u64, and this is the same word under the
 * name imported code uses. */
typedef struct { u64 pte; } pte_t;
static inline u64 pte_val(pte_t p) { return p.pte; }
static inline pte_t __pte(u64 v) { pte_t p = { .pte = v }; return p; }


/*
 * Kernel page protections, as pgprot_t values.
 *
 * The bits are b1nix's own PTE layout: present|write|global for ordinary kernel
 * memory, and the same plus cache-disable for memory-mapped I/O. PAT indexing
 * for write-combining goes through pgprot_writecombine() in <linux/mm.h>.
 */
#define PAGE_KERNEL     __pgprot((1ull << 0) | (1ull << 1) | (1ull << 63))
#define PAGE_KERNEL_IO  __pgprot((1ull << 0) | (1ull << 1) | (1ull << 4) | (1ull << 63))

/*
 * Building and installing a PTE by hand.
 *
 * Declared and deliberately not defined. b1nix's page tables are owned by its
 * VMM, which installs mappings through its own interface against a named
 * address space; there is no entry that takes a bare pte_t slot. A driver
 * reaching here — i915's remap_io_sg does — fails to link rather than writing
 * into a table nobody is tracking.
 */
struct mm_struct;
struct vm_area_struct;
pte_t pfn_pte(unsigned long pfn, pgprot_t prot);
pte_t pte_mkspecial(pte_t pte);
void set_pte_at(struct mm_struct *mm, unsigned long addr, pte_t *ptep, pte_t pte);

#endif
