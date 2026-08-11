/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_PNP_H
#define LKPI_LINUX_PNP_H
#include <linux/types.h>
/* ISA Plug-and-Play. Nothing on a Gen8/Gen9.5 platform is behind it; the
 * declarations exist because a probe path names the type. */
struct pnp_dev;

/* Has a PnP device claimed this I/O range? b1nix does not enumerate PnP
 * devices, so nothing has claimed anything and the range is reported free —
 * which is what lets the caller program it. On the M102 targets there is no
 * PnP-managed device competing for these ports. */
static inline int pnp_range_reserved(resource_size_t start, resource_size_t end)
{ (void)start; (void)end; return 0; }

#endif
