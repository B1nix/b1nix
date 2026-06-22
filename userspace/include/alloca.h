#ifndef _ALLOCA_H
#define _ALLOCA_H
/* <alloca.h>: stack allocation. b1nix has no separate alloca lib; the compiler
 * builtin provides it. Added for the Chromium port (M60-62). */
#include <stddef.h>
#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif
#endif /* _ALLOCA_H */
