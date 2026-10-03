/* SPDX-License-Identifier: GPL-2.0-only */
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
/* Read, build and replace a field in a value of a fixed width; @field is the
 * mask. */
#define __LKPI_MAKE_BITS_OPS(type)					\
static inline type type##_get_bits(type v, type field)			\
{ return (v & field) >> __bf_shf(field); }				\
static inline type type##_encode_bits(type v, type field)		\
{ return (v << __bf_shf(field)) & field; }				\
static inline type type##_replace_bits(type old, type val, type field)	\
{ return (old & ~field) | type##_encode_bits(val, field); }		\
static inline void type##p_replace_bits(type *p, type val, type field)	\
{ *p = type##_replace_bits(*p, val, field); }
__LKPI_MAKE_BITS_OPS(u8)
__LKPI_MAKE_BITS_OPS(u16)
__LKPI_MAKE_BITS_OPS(u32)
__LKPI_MAKE_BITS_OPS(u64)
#undef __LKPI_MAKE_BITS_OPS

#endif
