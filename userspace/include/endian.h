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

#endif /* _ENDIAN_H */
