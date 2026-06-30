#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H

/* Minimal <sys/prctl.h>. b1nix has no prctl syscall; the operations ports use
 * (thread/VMA naming) are cosmetic, so prctl() is a no-op that reports success.
 * Constants use the Linux ABI values so callers compile unchanged. */
#ifdef __cplusplus
extern "C" {
#endif

#define PR_SET_PDEATHSIG        1   /* Chromium port: prctl is a no-op success */
#define PR_GET_DUMPABLE         3
#define PR_SET_DUMPABLE         4
#define PR_SET_PTRACER 0x59616d61  /* Yama: allow this pid to ptrace us */
#define PR_SET_NAME            15
#define PR_GET_NAME            16
#define PR_SET_VMA             0x53564d41
#define PR_SET_VMA_ANON_NAME   0
/* Additional prctl option numbers (canonical Linux values). b1nix's prctl is a
 * no-op-success for most of these; they exist so code that *names* them — chiefly
 * the seccomp prctl-arg policy, dead on b1nix (--no-sandbox) — compiles with
 * distinct switch-case values. */
#define PR_SET_SECCOMP            22  /* M63: real — install a seccomp mode/filter */
#define PR_CAPBSET_READ           23
#define PR_SET_TIMERSLACK         29
#define PR_SET_NO_NEW_PRIVS       38  /* M63: real — forbid privilege escalation */
#define PR_GET_NO_NEW_PRIVS       39
#define PR_SET_THP_DISABLE        41
#define PR_MPX_ENABLE_MANAGEMENT  43
#define PR_SVE_GET_VL             51
#define PR_PAC_RESET_KEYS         54
#define PR_GET_TAGGED_ADDR_CTRL   56
#define PR_PAC_GET_ENABLED_KEYS   61
#define PR_SME_GET_VL             64

int prctl(int option, ...);

#ifdef __cplusplus
}
#endif
#endif
