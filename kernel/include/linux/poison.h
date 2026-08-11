/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_POISON_H
#define LKPI_LINUX_POISON_H
/* The byte patterns a debug build fills freed or in-use memory with, so that a
 * stale pointer dereferences something recognisable rather than plausible data.
 * b1nix's heap has its own canaries; these are the values imported code writes
 * itself, and the numbers are upstream's so a dump reads the same way. */
#define POISON_INUSE 0x5a
#define POISON_FREE  0x6b
#define POISON_END   0xa5
#endif
