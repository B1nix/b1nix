/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_RAID_XOR_H
#define LKPI_LINUX_RAID_XOR_H

#include <linux/types.h>

/*
 * The RAID-5 parity operation: XOR of up to five buffers into the first.
 *
 * Upstream picks between hand-written SIMD variants at boot; the interface is
 * the same either way and the arity is fixed at five because that is what the
 * generated code provides. btrfs's raid56 code calls the two- and three-buffer
 * forms.
 *
 * Like <linux/raid/pq.h>, the implementation is not imported yet and the same
 * consequence applies: raid5 and raid6 profiles do not work, every other
 * profile is unaffected.
 */

#define MAX_XOR_BLOCKS 4

void xor_blocks(unsigned int count, unsigned int bytes, void *dest, void **srcs);

#endif
