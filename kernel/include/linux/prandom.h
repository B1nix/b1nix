/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PRANDOM_H
#define LKPI_LINUX_PRANDOM_H
#include <linux/random.h>
/* Upstream folded the pseudo-random helpers back onto the real generator; so
 * does this. Nothing here needs a reproducible sequence. */
static inline u32 prandom_u32_max(u32 ceil) { return get_random_u32_below(ceil); }
#endif
