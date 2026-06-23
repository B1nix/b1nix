#ifndef _MACHINE_ENDIAN_H
#define _MACHINE_ENDIAN_H 1

/* BSD-style <machine/endian.h>.
 *
 * Some portable C/C++ code (e.g. LLVM's llvm/ADT/bit.h) reaches for
 * <machine/endian.h> on platforms it does not recognise as glibc/Linux. b1nix
 * defines __b1nix__/__unix__ (not __linux__), so it lands in that fallthrough.
 * Provide the BSD selectors here, consistent with <endian.h>. b1nix targets
 * x86/x86_64, which are little-endian. */

#include <endian.h>

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#endif
#ifndef PDP_ENDIAN
#define PDP_ENDIAN    __PDP_ENDIAN
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER    __BYTE_ORDER
#endif

#endif /* _MACHINE_ENDIAN_H */
