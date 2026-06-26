#ifndef B1NIX_U_ASM_UNISTD_64_H
#define B1NIX_U_ASM_UNISTD_64_H
/* On Linux this header defines the raw __NR_<name> syscall numbers. b1nix has
 * its own numbers, so we forward to <syscall.h>, which provides the full
 * __NR_<name> alias set mapped onto the b1nix syscall enum. Ports that
 * #include <asm/unistd_64.h> (e.g. WebRTC's platform_thread_types -> gettid)
 * then issue the CORRECT b1nix syscall, not a hardcoded Linux number. */
#include <syscall.h>
#endif
