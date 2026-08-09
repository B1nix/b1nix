/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_BITFIELD_H
#define LKPI_LINUX_BITFIELD_H
#include <linux/bits.h>
/* Pack and unpack a value into a mask's bit range. The shift is derived from
 * the mask rather than passed separately, which is what stops the two drifting
 * apart in a register definition. */
#define __bf_shf(mask) (__builtin_ffsll(mask) - 1)
#define FIELD_PREP(mask, val) (((__typeof__(mask))(val) << __bf_shf(mask)) & (mask))
#define FIELD_GET(mask, reg)  ((__typeof__(mask))(((reg) & (mask)) >> __bf_shf(mask)))
#define FIELD_FIT(mask, val)  (!(((val) << __bf_shf(mask)) & ~(mask)))
#endif
