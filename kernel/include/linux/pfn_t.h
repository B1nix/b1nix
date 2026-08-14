/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PFN_T_H
#define LKPI_LINUX_PFN_T_H
#include <linux/pfn.h>
#include <linux/types.h>
/* A frame number carried with flags saying what kind of memory it names. Only
 * the device-memory case appears here, and only as a value passed through. */
typedef struct { u64 val; } pfn_t;
#define PFN_DEV (1ULL << 62)
#define PFN_MAP (1ULL << 61)
static inline pfn_t __pfn_to_pfn_t(unsigned long pfn, u64 flags)
{ pfn_t p = { .val = (u64)pfn | flags }; return p; }
static inline pfn_t pfn_to_pfn_t(unsigned long pfn) { return __pfn_to_pfn_t(pfn, 0); }
static inline unsigned long pfn_t_to_pfn(pfn_t p)
{ return (unsigned long)(p.val & ~(PFN_DEV | PFN_MAP)); }
#endif
