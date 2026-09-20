#ifndef _ENDIAN_H
#define _ENDIAN_H 1

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

#define LITTLE_ENDIAN   __LITTLE_ENDIAN
#define BIG_ENDIAN      __BIG_ENDIAN
#define PDP_ENDIAN      __PDP_ENDIAN

/* b1nix targets x86 and x86_64, which are little endian. */
#define __BYTE_ORDER    __LITTLE_ENDIAN
#define BYTE_ORDER      __BYTE_ORDER

#include <stdint.h>

/* Byte-order conversion family (glibc <endian.h>). x86/x86_64 are
 * little-endian, so the host<->little funcs are identity and the host<->big
 * funcs are byte swaps. */
#define htobe16(x) __builtin_bswap16((uint16_t)(x))
#define htole16(x) ((uint16_t)(x))
#define be16toh(x) __builtin_bswap16((uint16_t)(x))
#define le16toh(x) ((uint16_t)(x))

#define htobe32(x) __builtin_bswap32((uint32_t)(x))
#define htole32(x) ((uint32_t)(x))
#define be32toh(x) __builtin_bswap32((uint32_t)(x))
#define le32toh(x) ((uint32_t)(x))

#define htobe64(x) __builtin_bswap64((uint64_t)(x))
#define htole64(x) ((uint64_t)(x))
#define be64toh(x) __builtin_bswap64((uint64_t)(x))
#define le64toh(x) ((uint64_t)(x))

#endif /* _ENDIAN_H */
