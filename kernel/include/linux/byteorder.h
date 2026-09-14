/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BYTEORDER_H
#define LKPI_LINUX_BYTEORDER_H
#include <linux/types.h>
/* x86 is little-endian, so the little-endian conversions are identities and the
 * big-endian ones are real swaps. Spelled out rather than aliased, because the
 * names say which byte order the *data* is in and that stays true elsewhere. */
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
#define cpu_to_le64(x) ((__le64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_be16(x) ((__be16)__builtin_bswap16((u16)(x)))
#define cpu_to_be32(x) ((__be32)__builtin_bswap32((u32)(x)))
#define be16_to_cpu(x) ((u16)__builtin_bswap16((u16)(x)))
#define be32_to_cpu(x) ((u32)__builtin_bswap32((u32)(x)))
#define cpu_to_be64(x) ((__be64)__builtin_bswap64((u64)(x)))
#define be64_to_cpu(x) ((u64)__builtin_bswap64((u64)(x)))
/* The pointer forms. They exist separately because the value forms take an
 * already-loaded value, and a filesystem reading a field out of a disk buffer
 * has a pointer into it — dereferencing through the right type is what keeps
 * the load the right width. */
#define le16_to_cpup(p) le16_to_cpu(*(const __le16 *)(p))
#define le32_to_cpup(p) le32_to_cpu(*(const __le32 *)(p))
#define le64_to_cpup(p) le64_to_cpu(*(const __le64 *)(p))
#define be16_to_cpup(p) be16_to_cpu(*(const __be16 *)(p))
#define be32_to_cpup(p) be32_to_cpu(*(const __be32 *)(p))
#define be64_to_cpup(p) be64_to_cpu(*(const __be64 *)(p))
/* Byte swaps, spelled the way imported code spells them. */
#define swab16(x) __builtin_bswap16((u16)(x))
#define swab32(x) __builtin_bswap32((u32)(x))
#define swab64(x) __builtin_bswap64((u64)(x))

/*
 * Add to a little-endian field in place.
 *
 * The pattern is read, convert, add, convert back, store — and it is written
 * out here rather than left to each caller because doing it by hand is where a
 * missing conversion hides: on a little-endian machine the wrong version works
 * anyway, so the bug only appears elsewhere.
 */
static inline void le16_add_cpu(__le16 *var, u16 val)
{ *var = cpu_to_le16(le16_to_cpu(*var) + val); }
static inline void le32_add_cpu(__le32 *var, u32 val)
{ *var = cpu_to_le32(le32_to_cpu(*var) + val); }
static inline void le64_add_cpu(__le64 *var, u64 val)
{ *var = cpu_to_le64(le64_to_cpu(*var) + val); }
static inline void be32_add_cpu(__be32 *var, u32 val)
{ *var = cpu_to_be32(be32_to_cpu(*var) + val); }

/* get_unaligned_* / put_unaligned_*: the upstream helpers, as functions. */
#include <linux/unaligned.h>

#endif
