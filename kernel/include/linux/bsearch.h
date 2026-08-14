/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BSEARCH_H
#define LKPI_LINUX_BSEARCH_H
#include <linux/types.h>

/* Binary search over a sorted array. Written out rather than forwarded to the
 * kernel's own: b1nix's lib has no bsearch, and the callers here pass a
 * comparison function with Linux's signature. */
static inline void *bsearch(const void *key, const void *base, size_t num,
                            size_t size, int (*cmp)(const void *, const void *))
{
	const char *pivot;
	int result;

	while (num > 0) {
		pivot = (const char *)base + (num >> 1) * size;
		result = cmp(key, pivot);
		if (result == 0)
			return (void *)pivot;
		if (result > 0) {
			base = pivot + size;
			num--;
		}
		num >>= 1;
	}
	return 0;
}
#endif
