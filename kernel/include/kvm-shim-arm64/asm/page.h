/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_PAGE_H
#define KVM_SHIM_ARM64_ASM_PAGE_H
/* In assembly, the page geometry upstream's <asm/page.h> gives; in C,
 * linuxkpi's page definitions. */
#ifdef __ASSEMBLY__
#include <asm/page-def.h>
#else
#include_next <asm/page.h>
#endif
#endif
