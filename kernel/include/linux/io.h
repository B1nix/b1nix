/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_IO_H
#define LKPI_LINUX_IO_H
#include <lkpi/io.h>
#include <linux/types.h>
#include <linux/string.h>

/*
 * Copying to and from device memory.
 *
 * On x86 MMIO is ordinary loads and stores, so these are memcpy — but they stay
 * separate names because on another architecture they are not, and imported
 * code chose the spelling deliberately.
 */
static inline void memcpy_fromio(void *dst, const void *src, usize n)
{
	memcpy(dst, src, n);
}

static inline void memcpy_toio(void *dst, const void *src, usize n)
{
	memcpy(dst, src, n);
}

static inline void memset_io(void *dst, int c, usize n) { memset(dst, c, n); }
#endif
