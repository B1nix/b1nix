/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_UTIL_MACROS_H
#define LKPI_LINUX_UTIL_MACROS_H
#include <linux/kernel.h>

/*
 * find_closest: the index of the entry in a sorted array nearest to x.
 *
 * The midpoint test is written the way upstream writes it — comparing against
 * the sum of neighbours rather than against a computed average — because the
 * average would round, and a value exactly between two entries would then pick
 * a different neighbour than the caller's table was built for.
 */
#define find_closest(x, a, as)                                          \
	({                                                                  \
		typeof(as) __fc_i, __fc_as = (as) - 1;                          \
		typeof(x) __fc_x = (x);                                         \
		typeof(*a) const *__fc_a = (a);                                 \
		for (__fc_i = 0; __fc_i < __fc_as; __fc_i++) {                  \
			if (__fc_x <= (__fc_a[__fc_i] + __fc_a[__fc_i + 1]) / 2)    \
				break;                                                  \
		}                                                               \
		__fc_i;                                                         \
	})

#define find_closest_descending(x, a, as)                               \
	({                                                                  \
		typeof(as) __fc_i, __fc_as = (as) - 1;                          \
		typeof(x) __fc_x = (x);                                         \
		typeof(*a) const *__fc_a = (a);                                 \
		for (__fc_i = 0; __fc_i < __fc_as; __fc_i++) {                  \
			if (__fc_x >= (__fc_a[__fc_i] + __fc_a[__fc_i + 1]) / 2)    \
				break;                                                  \
		}                                                               \
		__fc_i;                                                         \
	})
#endif
