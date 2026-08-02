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
#define PTRACE_SYSCALL 24
#define PTRACE_GETREGS 12
#define PTRACE_SETREGS 13
#define PTRACE_GETFPREGS 14
#define PTRACE_SETFPREGS 15
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17
#define PTRACE_SETOPTIONS 0x4200
#define PTRACE_GETEVENTMSG 0x4201
#define PTRACE_GETSIGINFO 0x4202
#define PTRACE_GETREGSET 0x4204
#define PTRACE_SETREGSET 0x4205
#define PTRACE_SEIZE 0x4206
#define PTRACE_INTERRUPT 0x4207
#define PTRACE_LISTEN 0x4208

/* PTRACE_SETOPTIONS flags (Linux values). Every bit outside PTRACE_O_SUPPORTED
 * is rejected with EINVAL rather than silently ignored, so a tracer that needs
 * one finds out immediately.
 *
 * Not supported: PTRACE_O_TRACEVFORKDONE, TRACESECCOMP, SUSPEND_SECCOMP — no
 * consumer, and each would be a lie without the machinery behind it.
 *
 * PTRACE_O_TRACEEXIT parks a dying task before teardown; the registers it
 * reports are the frame recorded at that task's last syscall entry, the final
 * ring-3 state it genuinely had. A SIGKILL death skips the stop — nothing may
 * delay a kill. */
#define PTRACE_O_TRACESYSGOOD 0x00000001
#define PTRACE_O_TRACEFORK 0x00000002
#define PTRACE_O_TRACEVFORK 0x00000004
#define PTRACE_O_TRACECLONE 0x00000008
#define PTRACE_O_TRACEEXEC 0x00000010
#define PTRACE_O_TRACEEXIT 0x00000040
#define PTRACE_O_EXITKILL 0x00100000
#define PTRACE_O_SUPPORTED                                                     \
  (PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |          \
   PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT |             \
   PTRACE_O_EXITKILL)

/* Event codes reported in the wait status as (SIGTRAP | event << 8). */
#define PTRACE_EVENT_FORK 1
#define PTRACE_EVENT_VFORK 2
#define PTRACE_EVENT_CLONE 3
#define PTRACE_EVENT_EXEC 4
#define PTRACE_EVENT_EXIT 6
#define PTRACE_EVENT_STOP 128

/* ELF note types selecting a register set for PTRACE_[GS]ETREGSET. */
#define NT_PRSTATUS 1
#define NT_PRFPREG 2
#define NT_X86_XSTATE 0x202
/* XSAVE area b1nix reports for NT_X86_XSTATE: legacy FXSAVE region + header. */
#define B1NIX_XSTATE_SIZE 576

/* Linux x86_64 struct user_regs_struct, in its exact field order — this is
 * what PTRACE_GETREGS/SETREGS exchange with the tracer. */
struct user_regs_struct {
  u64 r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi;
  u64 orig_rax, rip, cs, eflags, rsp, ss;
  u64 fs_base, gs_base, ds, es, fs, gs;
};

/* Linux's user_fpregs_struct is byte-for-byte the 512-byte FXSAVE area, which
 * is exactly how the kernel stores a task's FPU state (task->fpu_state). It is
 * declared opaque here because nothing in the kernel interprets its fields —
 * PTRACE_GETFPREGS/NT_PRFPREG copy the area out verbatim. */
struct user_fpregs_struct {
  u8 fxsave[512];
};

/* struct iovec as PTRACE_GETREGSET/SETREGSET take it in `data`. */
struct ptrace_iovec {
  u64 iov_base;
  u64 iov_len;
};

/* The subset of siginfo_t that PTRACE_GETSIGINFO fills in. Laid out to match
 * the Linux x86_64 siginfo_t prefix (si_signo, si_errno, si_code, then the
 * union), so a tracer reading into its own siginfo_t sees the right fields. */
struct ptrace_siginfo {
  int si_signo;
  int si_errno;
  int si_code;
  int _pad0;
  u64 si_addr;
  u8 _pad[128 - 24];
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

/* Linux's ptrace_may_access: may the calling task inspect `t`? Root and
 * CAP_SYS_PTRACE always may; otherwise the caller must own the target. When the
 * yama ptrace_scope restriction is on (see procfs /proc/sys/kernel/yama), the
 * caller must additionally be an ancestor of the target or the tracer the
 * target declared via prctl(PR_SET_PTRACER). Returns 1 (allowed) or 0. */
int ptrace_may_access(struct task *t);

/* Declared-tracer support for prctl(PR_SET_PTRACER): `tracer_pid` is the pid
 * allowed to attach under a non-zero ptrace_scope, or PTRACE_ANY_TRACER for
 * "anyone", or 0 to withdraw the declaration. */
#define PTRACE_ANY_TRACER ((usize)-1)
/* prctl(2) option numbers for the declaration (Linux values). */
#define PR_SET_PTRACER 0x59616d61
#define PR_SET_PTRACER_ANY (-1)
int ptrace_set_declared_tracer(struct task *t, usize tracer_pid);

/* yama-style attach restriction: 0 = classic (ownership only), 1 = a tracer
 * must also be an ancestor or the declared tracer. /proc/sys/kernel/yama/
 * ptrace_scope reads and writes it. */
int ptrace_scope_get(void);
int ptrace_scope_set(int scope);

/* Read/write a byte range in another task's address space through its own page
 * tables. Used by ptrace's PEEK/POKE, by /proc/<pid>/mem and by /proc/<pid>/auxv.
 * Returns the number of bytes transferred, or a negative errno. */
isize ptrace_copy_from_task(struct task *t, u64 addr, void *dst, usize len);
isize ptrace_copy_to_task(struct task *t, u64 addr, const void *src, usize len);

/* ── ptrace events (PTRACE_SETOPTIONS) ──────────────────────────────────────
 * A tracee reports an event by parking at its next return to ring 3 with the
 * event code attached to the stop, so its tracer's waitpid sees
 * (SIGTRAP | event << 8) and PTRACE_GETEVENTMSG returns the associated value.
 *
 * ptrace_event_child: called from fork/clone once the child task exists. When
 * the tracer asked for the matching event, the child is attached to the same
 * tracer and stopped before it runs, and the parent reports the event with the
 * child's pid as the message. No-op for an untraced or uninterested parent.
 *
 * ptrace_event_exec: called after a successful execve.
 *
 * ptrace_stop_event: the event code of the tracee's current stop (0 = a plain
 * signal stop) — waitpid folds it into the status word. */
void ptrace_event_child(struct task *parent, struct task *child, int event);
void ptrace_event_exec(struct task *t);
int ptrace_stop_event(struct task *t);

/* Non-zero while any tracee exists anywhere. The syscall hot path tests this
 * plain load before doing anything else, so an untraced system pays a single
 * memory read per syscall. */
int ptrace_any_traced(void);

/* Syscall-entry / syscall-exit trace stop (PTRACE_SYSCALL). `is_exit` selects
 * which of the pair this is. The reported stop signal is SIGTRAP, or
 * SIGTRAP|0x80 when the tracer set PTRACE_O_TRACESYSGOOD — that bit is how a
 * tracer tells a syscall stop from a genuine SIGTRAP. Also records the user
 * frame so an exit stop (below) has real registers to report. */
void ptrace_syscall_stop(struct task *t, struct interrupt_frame *frame,
                         int is_exit);

/* PTRACE_O_TRACEEXIT: park a dying task before its teardown so its tracer can
 * read the exit status (PTRACE_GETEVENTMSG) and its final register set — which
 * is the frame recorded at its last syscall entry, the last ring-3 state that
 * genuinely existed. Returns immediately for an untraced or uninterested task,
 * and is never used for a SIGKILL death (nothing may delay that). */
void ptrace_exit_stop(struct task *t, int exit_code);

/* Crash capture: the CPU-fault handler records what killed a task (signal,
 * faulting address, Linux si_code) so the faulting process's own SA_SIGINFO
 * handler and a tracer's PTRACE_GETSIGINFO both report it. ptrace_fault_info
 * returns 1 when a record exists for `t`. */
void ptrace_record_fault(struct task *t, int signo, u64 addr, int code);
int ptrace_fault_info(struct task *t, int *signo, u64 *addr, int *code);

/* Linux si_code values for the fault signals. */
#define B1NIX_SEGV_MAPERR 1
#define B1NIX_SEGV_ACCERR 2
#define B1NIX_SI_KERNEL 0x80

int ptrace_is_traced(struct task *t);
/* pid of the task tracing `t`, or 0 — waitpid uses it to report a tracee's
 * stops to a tracer that is not the tracee's parent. */
usize ptrace_tracer_pid(struct task *t);
void ptrace_task_cleanup(struct task *t);

#endif
