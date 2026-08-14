/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PREFETCH_H
#define LKPI_LINUX_PREFETCH_H
/* A hint, and nothing observable depends on it — but a real one: clang lowers
 * this to the prefetch instruction, so a list walk that asked for it gets it. */
static inline void prefetch(const void *x) { __builtin_prefetch(x, 0, 3); }
static inline void prefetchw(const void *x) { __builtin_prefetch(x, 1, 3); }
#define spin_lock_prefetch(x) prefetchw(x)
#endif
