#ifndef B1NIX_PTRACE_H
#define B1NIX_PTRACE_H

#include <b1nix/types.h>

struct task;
struct interrupt_frame;

/* ptrace(2) requests (values from <sys/ptrace.h>). */
#define PTRACE_TRACEME 0
#define PTRACE_PEEKTEXT 1
#define PTRACE_PEEKDATA 2
#define PTRACE_POKETEXT 4
#define PTRACE_POKEDATA 5
#define PTRACE_CONT 7
#define PTRACE_KILL 8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS 12
#define PTRACE_SETREGS 13
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17

/* Linux x86_64 struct user_regs_struct, in its exact field order — this is
 * what PTRACE_GETREGS/SETREGS exchange with the tracer. */
struct user_regs_struct {
  u64 r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi;
  u64 orig_rax, rip, cs, eflags, rsp, ss;
  u64 fs_base, gs_base, ds, es, fs, gs;
};

/* The syscall entry point. `data`/`addr` follow the ptrace(2) contract; the
 * result of a PEEK is returned through out_peek because a valid word can look
 * like an error code. */
isize ptrace_request(long request, usize pid, u64 addr, u64 data,
                     u64 *out_peek);

/* Called from the signal-delivery path: if the task is traced, stop it and let
 * its tracer inspect it. Returns 1 when the signal was consumed by the stop
 * (the caller must not deliver it), 0 otherwise. */
int ptrace_signal_stop(struct task *t, int signo, struct interrupt_frame *frame);

/* Debug exception (#DB) after PTRACE_SINGLESTEP: returns 1 if it belonged to a
 * traced task and was handled by stopping it. */
int ptrace_handle_debug_trap(struct interrupt_frame *frame);

int ptrace_is_traced(struct task *t);
void ptrace_task_cleanup(struct task *t);

#endif
