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
/* Added for the Chromium port (M60-62): more lowercase Linux SYS_* aliases used
 * by abseil raw-syscall paths. These map onto the matching b1nix syscalls, so
 * the raw syscall() calls hit the correct b1nix entry. */
#define SYS_write SYS_WRITE
#define SYS_read SYS_READ
#define SYS_rt_sigprocmask SYS_SIGPROCMASK
#define SYS_sigprocmask SYS_SIGPROCMASK

/* __NR_* aliases (the other Linux spelling). Only define the ones b1nix
 * actually implements natively, so raw syscall(__NR_x, ...) hits the correct
 * b1nix entry (argument order matches Linux). Calls that b1nix lacks
 * (sched_setattr, perf_event_open, mseal, ...) are left UNDEFINED on purpose:
 * Chromium guards them with `#ifdef __NR_x` and falls back gracefully. Added
 * for the Chromium port (M60-62): mojo channel_linux uses raw memfd/eventfd2. */
#define __NR_memfd_create SYS_MEMFD_CREATE
#define __NR_eventfd2     SYS_EVENTFD2
#define __NR_getdents64   SYS_GETDENTS64
#define __NR_getpid       SYS_GETPID
#define __NR_gettid       SYS_GETTID
#define __NR_getrandom    SYS_GETRANDOM
#define __NR_write        SYS_WRITE
#define __NR_read         SYS_READ
