/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_VT_H
#define LKPI_LINUX_VT_H
/* The virtual-terminal interface, used by fbdev emulation to know whether the
 * console owns the display. b1nix has VTs (M107) but they are not wired to the
 * imported console layer, and fbdev emulation is not built. */
#define MAX_NR_CONSOLES 63
#endif
