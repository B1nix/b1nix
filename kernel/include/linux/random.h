/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_RANDOM_H
#define LKPI_LINUX_RANDOM_H
#include <linux/types.h>

/* b1nix's kernel entropy source, named the way Linux names it. Declared rather
 * than inlined so this header does not drag <b1nix/random.h> — and its own
 * spelling of the same names — into every imported translation unit. */
u32 lkpi_random_u32(void);

static inline u32 get_random_u32(void) { return lkpi_random_u32(); }
static inline u64 get_random_u64(void)
{ return ((u64)lkpi_random_u32() << 32) | lkpi_random_u32(); }
static inline void get_random_bytes(void *buf, int nbytes)
{
	u8 *p = buf;
	while (nbytes >= 4) { u32 v = lkpi_random_u32(); __builtin_memcpy(p, &v, 4); p += 4; nbytes -= 4; }
	while (nbytes-- > 0) *p++ = (u8)lkpi_random_u32();
}
/* Upstream's "below this bound" helper. Modulo, so the distribution is very
 * slightly skewed for bounds that do not divide 2^32 — the callers here are
 * picking jitter and back-off, not keys. */
static inline u32 get_random_u32_below(u32 ceil)
{ return ceil ? lkpi_random_u32() % ceil : 0; }
#endif
