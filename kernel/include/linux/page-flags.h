/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PAGE_FLAGS_H
#define LKPI_LINUX_PAGE_FLAGS_H

#include <linux/mm.h>

/*
 * Page flags.
 *
 * The definitions live in <linux/mm.h> alongside `struct page` itself, because
 * the flags are a field of it and splitting the two means either header can be
 * included without the other and get half the picture. This file exists because
 * imported code includes it by name.
 */

#endif
