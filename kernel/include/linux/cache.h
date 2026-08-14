/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CACHE_H
#define LKPI_LINUX_CACHE_H
/* 64 bytes on every x86_64 part b1nix runs on. Used for alignment attributes,
 * where being wrong costs sharing rather than correctness — but two structures
 * sharing a line under a spinlock is exactly the cost a driver added this
 * attribute to avoid. */
#define L1_CACHE_SHIFT 6
#define L1_CACHE_BYTES (1 << L1_CACHE_SHIFT)
#define SMP_CACHE_BYTES L1_CACHE_BYTES

#define ____cacheline_aligned __attribute__((aligned(L1_CACHE_BYTES)))
#define __cacheline_aligned ____cacheline_aligned
#define ____cacheline_aligned_in_smp ____cacheline_aligned
#define __cacheline_aligned_in_smp ____cacheline_aligned
#define __read_mostly
#endif
