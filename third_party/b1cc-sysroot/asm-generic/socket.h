#ifndef B1NIX_U_ASM_GENERIC_SOCKET_H
#define B1NIX_U_ASM_GENERIC_SOCKET_H
/* On Linux this provides the SOL_SOCKET-level SO_* option numbers. b1nix keeps
 * them in <sys/socket.h> (Linux ABI values), so forward there; ports that
 * #include <asm-generic/socket.h> (e.g. WebRTC physical_socket_server) get the
 * b1nix definitions. */
#include <sys/socket.h>
#endif
