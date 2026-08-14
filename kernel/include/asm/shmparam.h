/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_SHMPARAM_H
#define LKPI_ASM_SHMPARAM_H
/* Cache-aliasing granularity for shared mappings. x86 caches are physically
 * indexed, so two virtual addresses for one page never alias and the value is
 * simply the page size. */
#define SHMLBA 4096
#endif
