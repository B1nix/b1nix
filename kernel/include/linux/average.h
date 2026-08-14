/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_AVERAGE_H
#define LKPI_LINUX_AVERAGE_H
#include <linux/types.h>
/* Exponentially weighted moving average, kept in fixed point so it needs no
 * floating point — which the kernel does not have. */
#define DECLARE_EWMA(name, prec, weight_rcp)                          \
	struct ewma_##name { unsigned long internal; };                   \
	static inline void ewma_##name##_init(struct ewma_##name *e)      \
	{ e->internal = 0; }                                              \
	static inline unsigned long ewma_##name##_read(struct ewma_##name *e) \
	{ return e->internal >> (prec); }                                 \
	static inline void ewma_##name##_add(struct ewma_##name *e,       \
	                                     unsigned long val)           \
	{                                                                 \
		unsigned long i = e->internal;                                \
		e->internal = i ? (((i << (weight_rcp)) - i) +                \
		                   (val << (prec))) >> (weight_rcp)           \
		                : (val << (prec));                            \
	}
#endif
