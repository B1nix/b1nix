#pragma once

/*
 * M63: seccomp-bpf — syscall filtering via a classic-BPF program attached to a
 * task. A process installs an immutable filter; on every syscall the kernel runs
 * the filter over a `struct seccomp_data` (syscall nr + args) and acts on the
 * returned verdict (allow / fail with errno / kill). Filters are inherited
 * across fork and survive execve, and (like Linux) cannot be removed once set —
 * a child can only ever be more restricted than its parent.
 *
 * This is the bounded, self-contained part of M63; user/PID/net namespaces and
 * the setuid-sandbox helper remain deferred (see the roadmap).
 */

#include <b1nix/types.h>

struct interrupt_frame;
struct task;

/* Classic BPF instruction (Linux struct sock_filter, 8 bytes). */
struct sock_filter {
  u16 code;
  u8 jt;
  u8 jf;
  u32 k;
};

/* Input the filter program runs over (Linux struct seccomp_data). */
struct seccomp_data {
  int nr;                  /* syscall number */
  u32 arch;                /* AUDIT_ARCH_* of the caller */
  u64 instruction_pointer; /* userspace RIP at the syscall */
  u64 args[6];             /* syscall arguments */
};

/* prctl / seccomp() operation + mode values (Linux ABI). */
#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT   1
#define SECCOMP_MODE_FILTER   2

#define SECCOMP_SET_MODE_STRICT 0
#define SECCOMP_SET_MODE_FILTER 1

#define PR_SET_SECCOMP 22
#define PR_GET_SECCOMP 21
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39

/* Filter verdict: high 16 bits select the action, low bits carry data. */
#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_KILL_THREAD  0x00000000u
#define SECCOMP_RET_KILL         SECCOMP_RET_KILL_THREAD
#define SECCOMP_RET_TRAP         0x00030000u
#define SECCOMP_RET_ERRNO        0x00050000u
#define SECCOMP_RET_TRACE        0x7ff00000u
#define SECCOMP_RET_LOG          0x7ffc0000u
#define SECCOMP_RET_ALLOW        0x7fff0000u
#define SECCOMP_RET_ACTION_FULL  0xffff0000u
#define SECCOMP_RET_DATA         0x0000ffffu

/* AUDIT_ARCH for the seccomp_data.arch field. */
#define AUDIT_ARCH_X86_64 0xC000003Eu
#define AUDIT_ARCH_I386   0x40000003u

/* struct sock_fprog passed by userspace to install a filter. */
struct sock_fprog {
  u16 len;
  struct sock_filter *filter;
};

/* Install a filter for the current task (SECCOMP_SET_MODE_FILTER). Returns 0 or
 * -errno. `prog` points at a userspace struct sock_fprog. */
int seccomp_set_mode_filter(u32 flags, const void *user_prog);
/* Enter SECCOMP_MODE_STRICT (only read/write/exit/sigreturn permitted). */
int seccomp_set_mode_strict(void);
/* prctl(PR_SET_NO_NEW_PRIVS,...) gate (Linux requires it before filter install
 * unless privileged). b1nix tracks it but does not require it. */
int seccomp_set_no_new_privs(void);
int seccomp_get_no_new_privs(void);

/* Run the calling task's filter (if any) over a syscall. Returns:
 *   0          -> allow (proceed with the syscall)
 *   negative   -> the syscall must return this value (-errno) without running
 * Kills the task directly (no return) on a KILL verdict. */
isize seccomp_filter_syscall(u64 number, u64 a0, u64 a1, u64 a2, u64 a3,
                             u64 a4, u64 a5, struct interrupt_frame *frame);

/* True if the task has any seccomp restriction installed (fast gate so the
 * common no-filter path costs a single load). */
int seccomp_active(void);

/* fork/exec hooks: filters are inherited by the child and survive exec. */
void seccomp_inherit(struct task *parent, struct task *child);
