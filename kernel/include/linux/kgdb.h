/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_KGDB_H
#define LKPI_LINUX_KGDB_H
#include <linux/types.h>
/* b1nix has a GDB stub (kernel/arch/x86_64/gdbstub.c), but the core only uses
 * kgdb here to ask "is a debugger attached, should I break instead of print".
 * Reporting no is correct: nothing attaches the stub to the DRM paths. */
static inline int kgdb_connected_stub(void) { return 0; }
#define in_dbg_master() 0
static inline void kgdb_breakpoint(void) { }
#endif
