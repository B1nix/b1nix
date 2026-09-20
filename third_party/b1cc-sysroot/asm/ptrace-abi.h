#ifndef _ASM_X86_PTRACE_ABI_H
#define _ASM_X86_PTRACE_ABI_H

/* Minimal <asm/ptrace-abi.h> for b1nix.
 *
 * b1nix has no ptrace(2) (returns ENOSYS). Chromium's seccomp helpers include
 * this for the PTRACE_* request constants used in syscall-argument restriction
 * policies — all dead code on b1nix (--no-sandbox). Standard Linux values so the
 * (never-run) policy logic is internally consistent. Real ptrace = M80. */

#define PTRACE_TRACEME             0
#define PTRACE_PEEKTEXT            1
#define PTRACE_PEEKDATA            2
#define PTRACE_PEEKUSR             3
#define PTRACE_POKETEXT            4
#define PTRACE_POKEDATA            5
#define PTRACE_POKEUSR             6
#define PTRACE_CONT                7
#define PTRACE_KILL                8
#define PTRACE_SINGLESTEP          9
#define PTRACE_GETREGS             12
#define PTRACE_SETREGS             13
#define PTRACE_GETFPREGS           14
#define PTRACE_SETFPREGS           15
#define PTRACE_ATTACH              16
#define PTRACE_DETACH              17
#define PTRACE_GETFPXREGS          18
#define PTRACE_SETFPXREGS          19
#define PTRACE_SYSCALL             24
#define PTRACE_GET_THREAD_AREA     25
#define PTRACE_SET_THREAD_AREA     26
/* ARM-only requests; defined so x86_64 policy tables that list them compile. */
#define PTRACE_GETVFPREGS          27
#define PTRACE_GETHBPREGS          29
#define PTRACE_ARCH_PRCTL          30
#define PTRACE_SYSEMU              31
#define PTRACE_SYSEMU_SINGLESTEP   32
#define PTRACE_GETREGSET           0x4204
#define PTRACE_SETREGSET           0x4205
#define PTRACE_SEIZE               0x4206
#define PTRACE_INTERRUPT           0x4207
#define PTRACE_LISTEN              0x4208
#define PTRACE_PEEKSIGINFO         0x4209
#define PTRACE_GETSIGMASK          0x420a
#define PTRACE_SETSIGMASK          0x420b

#endif /* _ASM_X86_PTRACE_ABI_H */
