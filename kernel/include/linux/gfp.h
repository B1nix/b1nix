/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_GFP_H
#define LKPI_LINUX_GFP_H
#include <lkpi/types.h>
#include <linux/types.h>
/* Allocation flags. b1nix's kmalloc never sleeps and never blocks, so the
 * distinction that matters on Linux — may this allocation sleep — does not
 * exist here; the flags are accepted so callers compile, and __GFP_ZERO is
 * honoured because it changes the result. */
#define __GFP_NOWARN  0x0200u
#define __GFP_NOFAIL  0x1000u
#define __GFP_NORETRY 0x4000u
#define __GFP_ZERO_ALIAS __GFP_ZERO
#define __GFP_COMP    0x2000u
#define __GFP_NOFAIL  0x1000u
#define __GFP_COMP    0x2000u
#define __GFP_RETRY_MAYFAIL 0x0400u
#define __GFP_HIGHMEM 0x0800u
#define __GFP_DMA32   0x1000u
#define GFP_USER      GFP_KERNEL
#define GFP_HIGHUSER  GFP_KERNEL
#endif
