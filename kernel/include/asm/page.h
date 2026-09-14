/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_PAGE_H
#define LKPI_ASM_PAGE_H

/*
 * The page geometry, which architecture defines and <lkpi/page.h> already
 * settles for this kernel. This header exists because imported code spells the
 * include as <asm/page.h>; both spellings must reach the same numbers.
 */
#include <lkpi/page.h>

#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#ifndef PAGE_MASK
#define PAGE_MASK (~((unsigned long)PAGE_SIZE - 1))
#endif

#endif
