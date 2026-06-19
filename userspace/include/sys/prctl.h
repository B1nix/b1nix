#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H

/* Minimal <sys/prctl.h>. b1nix has no prctl syscall; the operations ports use
 * (thread/VMA naming) are cosmetic, so prctl() is a no-op that reports success.
 * Constants use the Linux ABI values so callers compile unchanged. */
#ifdef __cplusplus
extern "C" {
#endif

#define PR_SET_NAME            15
#define PR_GET_NAME            16
#define PR_SET_VMA             0x53564d41
#define PR_SET_VMA_ANON_NAME   0

int prctl(int option, ...);

#ifdef __cplusplus
}
#endif
#endif
