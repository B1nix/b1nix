/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MMAN_H
#define LKPI_LINUX_MMAN_H

#include <linux/types.h>

/* Protection and sharing bits for a mapping, in the numbering the architecture
 * fixes. Guarded because b1nix's own <b1nix/mm.h> defines the same names for
 * the same values, and a file that bridges the two would otherwise redefine
 * them. */
#ifndef PROT_READ
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif
#ifndef MAP_SHARED
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#endif

#endif
