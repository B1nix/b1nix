/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PFN_H
#define LKPI_LINUX_PFN_H
#include <linux/kernel.h> /* PAGE_SHIFT */
#include <linux/types.h>
#include <lkpi/page.h>   /* PAGE_SIZE */

#define PFN_ALIGN(x) (((unsigned long)(x) + (PAGE_SIZE - 1)) & ~((unsigned long)PAGE_SIZE - 1))
#define PFN_UP(x)    (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_DOWN(x)  ((x) >> PAGE_SHIFT)
#define PFN_PHYS(x)  ((phys_addr_t)(x) << PAGE_SHIFT)
#define PHYS_PFN(x)  ((unsigned long)((x) >> PAGE_SHIFT))
#endif
