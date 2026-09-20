/* linux/mman.h - b1nix compatibility shim for musl userspace.
 *
 * The Linux UAPI header linux/mman.h exposes the MAP, MADV and MCL flag
 * constants that some ports (OpenSSL secure-heap code) include directly.
 * Under musl these all live in sys/mman.h, so pull that in and fill any
 * gaps the UAPI header historically added. -idirafter keeps this at lowest
 * include priority, so it only resolves the linux/mman.h spelling. */
#ifndef _B1NIX_LINUX_MMAN_H
#define _B1NIX_LINUX_MMAN_H

#include <sys/mman.h>

#ifndef MADV_DONTDUMP
#define MADV_DONTDUMP 16
#endif
#ifndef MADV_DODUMP
#define MADV_DODUMP 17
#endif
#ifndef MAP_LOCKED
#define MAP_LOCKED 0x2000
#endif
#ifndef MCL_CURRENT
#define MCL_CURRENT 1
#endif
#ifndef MCL_FUTURE
#define MCL_FUTURE 2
#endif
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif

#endif /* _B1NIX_LINUX_MMAN_H */
