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
/* Byte swaps, spelled the way imported code spells them. */
#define swab16(x) __builtin_bswap16((u16)(x))
#define swab32(x) __builtin_bswap32((u32)(x))
#define swab64(x) __builtin_bswap64((u64)(x))

#define get_unaligned_le16(p) le16_to_cpu(*(const __le16 *)(p))
#define get_unaligned_le32(p) le32_to_cpu(*(const __le32 *)(p))
#endif
