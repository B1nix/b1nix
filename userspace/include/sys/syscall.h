#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H
/* Linux puts the SYS_* numbers + syscall() here; b1nix keeps them in
 * <syscall.h>. Re-export so ports that include <sys/syscall.h> compile.
 * b1nix-specific names are uppercase (SYS_GETTID); Linux-only probes like
 * `#ifdef SYS_kcmp` simply stay undefined, which is the intended fallback. */
#include <syscall.h>
#endif
