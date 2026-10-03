/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TBOOT_H
#define LKPI_LINUX_TBOOT_H
/* Intel TXT's measured launch is never used to start this kernel. */
static inline int tboot_enabled(void) { return 0; }
#endif
