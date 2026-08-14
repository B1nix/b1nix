/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KFIFO_H
#define LKPI_LINUX_KFIFO_H
#include <linux/kernel.h>
#include <linux/slab.h>
/* A power-of-two ring. The size being a power of two is what makes the index
 * arithmetic a mask instead of a modulo, and is why the API insists on it. */
#define DECLARE_KFIFO(name, type, size) \
	struct { type buf[size]; unsigned int in, out; } name
#define INIT_KFIFO(name) do { (name).in = 0; (name).out = 0; } while (0)
#define kfifo_len(f)   ((f)->in - (f)->out)
#define kfifo_is_empty(f) ((f)->in == (f)->out)
#define kfifo_is_full(f)  (kfifo_len(f) >= ARRAY_SIZE((f)->buf))
#define kfifo_put(f, v)                                            \
	({                                                             \
		int __ok = !kfifo_is_full(f);                              \
		if (__ok)                                                  \
			(f)->buf[(f)->in++ & (ARRAY_SIZE((f)->buf) - 1)] = (v); \
		__ok;                                                      \
	})
#define kfifo_get(f, vp)                                           \
	({                                                             \
		int __ok = !kfifo_is_empty(f);                             \
		if (__ok)                                                  \
			*(vp) = (f)->buf[(f)->out++ & (ARRAY_SIZE((f)->buf) - 1)]; \
		__ok;                                                      \
	})
#endif
