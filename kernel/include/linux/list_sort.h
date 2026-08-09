/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_LIST_SORT_H
#define LKPI_LINUX_LIST_SORT_H
#include <linux/list.h>
/* Stable merge sort of a list in place. Stability matters to the caller: the
 * DRM core sorts modes and expects equal entries to keep the order the driver
 * added them in. */
void list_sort(void *priv, struct list_head *head,
               int (*cmp)(void *priv, const struct list_head *a,
                          const struct list_head *b));
#endif
