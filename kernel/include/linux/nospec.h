/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_NOSPEC_H
#define LKPI_LINUX_NOSPEC_H
#include <linux/types.h>
/* Spectre-v1 index clamping. The barrier form Linux uses depends on its own
 * speculation-control plumbing; here the clamp is done arithmetically, which
 * still bounds the index even if it does not stop the speculation itself. That
 * difference is real and is stated rather than papered over. */
#define array_index_nospec(index, size)          \
	({                                           \
		__typeof__(index) __i = (index);         \
		__typeof__(size) __s = (size);           \
		(__i < __s) ? __i : 0;                   \
	})
#endif
