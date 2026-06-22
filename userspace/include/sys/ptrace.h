#ifndef _SYS_PTRACE_H
#define _SYS_PTRACE_H

#include <stdint.h>

/* <sys/ptrace.h>: process-trace request codes and the ptrace() prototype.
 * b1nix does not implement ptrace(2) (returns -ENOSYS), but ports such as
 * Chromium's third_party/lss include this header for the PTRACE_* request
 * enum and the prototype. Values match the Linux UAPI. Added for the
 * Chromium port (M60-62). */

#ifdef __cplusplus
extern "C" {
#endif

enum __ptrace_request {
  PTRACE_TRACEME          = 0,
  PTRACE_PEEKTEXT         = 1,
  PTRACE_PEEKDATA         = 2,
  PTRACE_PEEKUSER         = 3,
  PTRACE_POKETEXT         = 4,
  PTRACE_POKEDATA         = 5,
  PTRACE_POKEUSER         = 6,
  PTRACE_CONT             = 7,
  PTRACE_KILL             = 8,
  PTRACE_SINGLESTEP       = 9,
  PTRACE_GETREGS          = 12,
  PTRACE_SETREGS          = 13,
  PTRACE_GETFPREGS        = 14,
  PTRACE_SETFPREGS        = 15,
  PTRACE_ATTACH           = 16,
  PTRACE_DETACH           = 17,
  PTRACE_GETFPXREGS       = 18,
  PTRACE_SETFPXREGS       = 19,
  PTRACE_SYSCALL          = 24,
  PTRACE_SETOPTIONS       = 0x4200,
  PTRACE_GETEVENTMSG      = 0x4201,
  PTRACE_GETSIGINFO       = 0x4202,
  PTRACE_SETSIGINFO       = 0x4203,
  PTRACE_GETREGSET        = 0x4204,
  PTRACE_SETREGSET        = 0x4205,
  PTRACE_SEIZE            = 0x4206,
  PTRACE_INTERRUPT        = 0x4207,
  PTRACE_LISTEN           = 0x4208,
  PTRACE_PEEKSIGINFO      = 0x4209,
  PTRACE_GETSIGMASK       = 0x420a,
  PTRACE_SETSIGMASK       = 0x420b
};

/* ptrace(2). b1nix has no tracer; this returns -1/ENOSYS at runtime. */
long ptrace(int request, ...);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PTRACE_H */
