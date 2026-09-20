#ifndef _LINUX_NET_H
#define _LINUX_NET_H

/* Minimal <linux/net.h> for b1nix.
 *
 * On Linux this header carries the socketcall(2) multiplexer sub-op enum
 * (SYS_SOCKET, SYS_RECVMSG, ...). x86_64 has no socketcall syscall (it uses the
 * direct socket syscalls), so the only consumers — Chromium's seccomp helpers —
 * gate every use behind `#if defined(__NR_socketcall)`, which is never defined
 * for the x86_64 b1nix target. The header therefore only needs to exist; it
 * deliberately defines nothing, avoiding a clash with b1nix's own uppercase
 * SYS_* syscall enum in <syscall.h>. (The seccomp code is itself dead on b1nix,
 * which runs --no-sandbox; real seccomp/socketcall is roadmap M63.) */

#endif /* _LINUX_NET_H */
