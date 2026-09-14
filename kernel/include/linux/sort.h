/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SORT_H
#define LKPI_LINUX_SORT_H
#include <linux/types.h>
void sort(void *base, usize num, usize size,
          int (*cmp)(const void *, const void *),
          void (*swap_fn)(void *, void *, int));
void sort_r(void *base, usize num, usize size,
            int (*cmp)(const void *, const void *, const void *),
            void (*swap_fn)(void *, void *, int, const void *),
            const void *priv);
#endif
