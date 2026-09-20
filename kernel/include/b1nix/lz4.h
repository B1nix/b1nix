/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_LZ4_H
#define B1NIX_LZ4_H

#include <b1nix/types.h>

/*
 * LZ4 block format, the kernel's own implementation.
 *
 * Two subsystems compress pages: zswap keeps swapped-out pages in RAM
 * (kernel/mm/swap.c), and zram is a block device whose sectors live compressed
 * in RAM (kernel/dev/zram.c). They want the same codec and must not want the
 * same scratch table, so the hash table is the caller's: each passes its own
 * and serialises access to it however it already serialises itself.
 *
 * Pure integer code. No SSE, no floating point, no allocation -- it is called
 * from the eviction path, which runs when the machine has no memory to spare.
 */

/* 4096 entries, 8 KiB. The table is cleared at the start of every block, and
 * every caller here compresses exactly one 4 KiB page -- so a 64 KiB table
 * meant 64 KiB of memset per page, which at a few thousand pages of reclaim is
 * hundreds of megabytes of memset in the middle of a page fault. A page has
 * 4092 match positions; a table of 4096 is the size that fits it. */
#define LZ4_HASH_LOG 12
#define LZ4_HASH_ENTRIES (1u << LZ4_HASH_LOG)

/* The most a block of `n` bytes can grow to when nothing in it repeats. */
#define LZ4_COMPRESS_BOUND(n) ((n) + ((n) / 255) + 16)

/* Compress `src_size` bytes into at most `dst_cap`. Returns the compressed
 * size, or 0 when the result would not fit -- which the callers read as
 * "incompressible", not as an error. `scratch` is LZ4_HASH_ENTRIES u16s. */
int lz4_compress(const u8 *src, int src_size, u8 *dst, int dst_cap,
                 u16 *scratch);

/* Decode exactly `dst_size` bytes (raw-block convention: the last sequence is
 * literals only). Returns 0, or -1 on input that does not decode within
 * bounds. Every read from `src` and write to `dst` is checked: the input may
 * be a blob the caller no longer trusts. */
int lz4_decompress(const u8 *src, int src_size, u8 *dst, int dst_size);

#endif /* B1NIX_LZ4_H */
