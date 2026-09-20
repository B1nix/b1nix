/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * LZ4 block format. See kernel/include/b1nix/lz4.h for what this is for and
 * who calls it.
 *
 * Written here rather than imported: Linux's lib/lz4 is one of the files the
 * filesystem import already stages (it backs zstd's neighbours in the btrfs
 * tree), but the kernel needs a compressor on the eviction path long before
 * any filesystem is mounted, and the imported tree is fetched at build time
 * and may be absent. This is the format, not a variant of it: blobs written
 * here decode with any LZ4 implementation, which is what makes the compressed
 * pages readable by anything that ever has to look at them.
 */
#include <b1nix/lz4.h>
#include <string.h>

/* LZ4 block-format hash of the next 4 bytes. Unaligned-safe. */
static u32 lz4_hash4(const u8 *p) {
    u32 v;
    memcpy(&v, p, 4);
    v *= 0x9E3779B1u;
    return (v >> (32 - LZ4_HASH_LOG)) & ((1u << LZ4_HASH_LOG) - 1);
}

/* LZ4 block-format compressor (greedy, single pass). Returns the compressed
 * size (>0), or 0 when the input cannot be represented within dstCapacity
 * (treated as incompressible by the caller). Pure integer code — no SSE, no
 * floating point — safe for the kernel's -mno-sse build. The scratch hash
 * table belongs to the caller, which is what lets two subsystems compress at
 * the same time without sharing a lock. */
int lz4_compress(const u8 *src, int srcSize, u8 *dst, int dstCapacity,
                 u16 *scratch) {
    const int HT = 1 << LZ4_HASH_LOG;
    u16 *ht = scratch;

    if (!src || !dst || !ht || srcSize < 5 || dstCapacity <= 0)
        return 0;
    for (int i = 0; i < HT; i++) ht[i] = 0xFFFF;

    int ip = 0;      /* current position in src */
    int anchor = 0;  /* start of pending literals */
    int op = 0;      /* write position in dst */

    while (ip < srcSize - 4) {
        u32 h = lz4_hash4(src + ip);
        u16 cand = ht[h];
        ht[h] = (u16)ip;
        if (cand != 0xFFFF && ip - cand > 0 && ip - cand < 0x10000) {
            u32 a, b;
            memcpy(&a, src + cand, 4);
            memcpy(&b, src + ip, 4);
            if (a == b) {
                int len = 4;
                while (ip + len < srcSize && src[cand + len] == src[ip + len])
                    len++;
                int litLen = ip - anchor;
                int mlen = len - 4;
                if (op + 1 + litLen / 255 + litLen + 2 + mlen / 255 > dstCapacity)
                    return 0;
                int tok = op++;
                dst[tok] = (u8)((litLen >= 15 ? 15 : litLen) << 4);
                if (litLen >= 15) {
                    int e = litLen - 15;
                    while (e >= 255) { dst[op++] = 255; e -= 255; }
                    dst[op++] = (u8)e;
                }
                memcpy(dst + op, src + anchor, (usize)litLen);
                op += litLen;
                dst[tok] |= (u8)(mlen >= 15 ? 15 : mlen);
                int off = ip - cand;
                dst[op++] = (u8)off;
                dst[op++] = (u8)(off >> 8);
                if (mlen >= 15) {
                    int e = mlen - 15;
                    while (e >= 255) { dst[op++] = 255; e -= 255; }
                    dst[op++] = (u8)e;
                }
                ip += len;
                anchor = ip;
                continue;
            }
        }
        ip++;
    }

    /* Trailing literals — the last LZ4 sequence carries no match. */
    int litLen = srcSize - anchor;
    if (op + 1 + litLen / 255 + litLen > dstCapacity) return 0;
    dst[op++] = (u8)((litLen >= 15 ? 15 : litLen) << 4);
    if (litLen >= 15) {
        int e = litLen - 15;
        while (e >= 255) { dst[op++] = 255; e -= 255; }
        dst[op++] = (u8)e;
    }
    memcpy(dst + op, src + anchor, (usize)litLen);
    op += litLen;
    return op;
}

/* LZ4 block-format decompressor. Decodes exactly dstSize bytes (raw-block
 * convention: the final sequence is literals only). Returns 0 on success, -1
 * on corrupt input (bounds-checked against srcSize). */
int lz4_decompress(const u8 *src, int srcSize, u8 *dst, int dstSize) {
    if (!src || !dst || srcSize <= 0 || dstSize <= 0) return -1;
    int ip = 0;
    int op = 0;
    for (;;) {
        if (ip >= srcSize) return -1;
        u8 token = src[ip++];
        int litLen = token >> 4;
        if (litLen == 15) {
            for (;;) {
                if (ip >= srcSize) return -1;
                u8 b = src[ip++];
                litLen += b;
                if (b != 255) break;
            }
        }
        if (op + litLen > dstSize || ip + litLen > srcSize) return -1;
        memcpy(dst + op, src + ip, (usize)litLen);
        ip += litLen;
        op += litLen;
        if (op == dstSize) return 0;   /* block end: trailing literals */
        if (ip + 2 > srcSize) return -1;
        int off = src[ip] | (src[ip + 1] << 8);
        ip += 2;
        if (off == 0 || off > op) return -1;
        int mlen = (token & 0xF) + 4;
        if ((token & 0xF) == 15) {
            for (;;) {
                if (ip >= srcSize) return -1;
                u8 b = src[ip++];
                mlen += b;
                if (b != 255) break;
            }
        }
        if (op + mlen > dstSize) return -1;
        for (int i = 0; i < mlen; i++)   /* overlapping matches allowed */
            dst[op + i] = dst[op + i - off];
        op += mlen;
    }
}
