#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H
/* Linux puts the SYS_* numbers + syscall() here; b1nix keeps them in
 * <syscall.h>. Re-export so ports that include <sys/syscall.h> compile.
 * b1nix-specific names are uppercase (SYS_GETTID); Linux-only probes like
 * `#ifdef SYS_kcmp` simply stay undefined, which is the intended fallback. */
#include <syscall.h>
#endif

/* Linux lowercase aliases some ports use. */
#define SYS_gettid SYS_GETTID
#define SYS_futex SYS_FUTEX
#define SYS_getrandom SYS_GETRANDOM
/* Linux self-re-raise used by LLVM's crash signal handler. b1nix re-raises the
 * signal to the calling process (siginfo payload is not preserved). */
#define SYS_rt_tgsigqueueinfo SYS_RT_TGSIGQUEUEINFO
