/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_MATH_H
#define LKPI_LINUX_MATH_H
#include <linux/kernel.h>
#include <linux/types.h>

/* The integer helpers upstream moved out of <linux/kernel.h>. Rounding is the
 * only part with a trap in it: round_up assumes a power-of-two multiple and
 * roundup does not, and using the wrong one is silent. */
#ifndef abs_diff
#define abs_diff(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))
#endif
#ifndef roundup
#define roundup(x, y) (((((x) + ((y) - 1)) / (y))) * (y))
#endif
#ifndef rounddown
#define rounddown(x, y) (((x) / (y)) * (y))
#endif
#ifndef DIV_ROUND_DOWN_ULL
#define DIV_ROUND_DOWN_ULL(ll, d) ((unsigned long long)(ll) / (d))
#endif
#ifndef DIV_ROUND_UP_ULL
#define DIV_ROUND_UP_ULL(ll, d) DIV_ROUND_DOWN_ULL((ll) + (d) - 1, (d))
#endif
#endif
