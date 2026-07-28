#include <b1nix/arch_x86_64.h>
#include <b1nix/linux_abi.h>
#include <b1nix/ptrace.h>
#include <b1nix/rseq.h>
#include <b1nix/sched.h>
#include <b1nix/signal.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/arch.h>
#include <b1nix/klog.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <stdio.h>
#include <string.h>


static int is_valid_user_code_ptr(u64 ptr) {
  if (ptr == 0)
    return 0;
  return ptr < 0x0000800000000000ULL;
}

static void arch_build_signal_frame(struct interrupt_frame *frame, int sig,
                                    int si_code, union sigval si_val) {
  struct task *t = current_task;
  /* M74: RT signals (SIGRTMIN..SIGRTMAX) keep their sigaction in the side-table,
   * not the in-struct sigactions[31] array. */
  struct sigaction *sa = SIG_IS_RT(sig) ? scheduler_rt_action_current(sig)
                                        : &t->sigactions[sig - 1];
  if (!sa) {
    scheduler_exit_current(-SIGSEGV);
    return;
  }

  /* Preserve x86_64 SysV red zone (128 bytes below RSP). */
  u64 user_rsp = frame->rsp - 128;
  /* SA_ONSTACK: if the handler asked for the alternate signal stack and one is
   * registered, deliver the frame at the top of that stack instead — unless we
   * are already executing on it (POSIX: do not nest onto the alt stack). */
  if ((sa->sa_flags & SA_ONSTACK) && !task_on_altstack(t, frame->rsp)) {
    u64 alt_top = task_altstack_top(t);
    if (alt_top)
      user_rsp = alt_top;
  }
  struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
  /* SA_SIGINFO: the handler is the 3-arg form and needs a siginfo_t in RSI and a
   * ucontext_t in RDX. The Linux personality uses the Linux siginfo/ucontext
   * layout; native b1nix programs (M74 — RT signals / sigqueue / POSIX timers)
   * use the native siginfo_t (a null ucontext: the payload of interest is
   * si_value, which handlers read from the siginfo). Both are placed ABOVE the
   * sigframe so the handler's downward stack growth never clobbers them. */
  int is_siginfo = img && img->personality == PERSONALITY_LINUX &&
                   (sa->sa_flags & SA_SIGINFO);
  int is_native_siginfo = img && img->personality != PERSONALITY_LINUX &&
                          (sa->sa_flags & SA_SIGINFO);
  u64 si_addr = 0, uc_addr = 0;
  u64 top = user_rsp;
  if (is_native_siginfo) {
    si_addr = (top - sizeof(struct b1nix_native_siginfo)) & ~0xFULL;
    top = si_addr;
  } else if (is_siginfo) {
    si_addr = (top - sizeof(struct linux_siginfo)) & ~0xFULL;
    uc_addr = (si_addr - sizeof(struct linux_ucontext)) & ~0xFULL;
    top = uc_addr;
  }
  u64 frame_base = (top - sizeof(struct b1nix_sigframe)) & ~0xFULL;
  u64 restorer_slot = frame_base - sizeof(u64);

  /* Prefer the userspace-supplied sa_restorer (provided by musl/libc.so in Ring 3);
   * fall back to the kernel-owned trampoline if sa_restorer is NULL. */
  u64 restorer = (sa && sa->sa_restorer)
                     ? (u64)(usize)sa->sa_restorer
                     : (img ? img->sigreturn_trampoline : 0);
  if (!is_valid_user_code_ptr((u64)(usize)sa->sa_handler) ||
      !is_valid_user_code_ptr(restorer)) {
    scheduler_exit_current(-SIGSEGV);
    return;
  }

  struct b1nix_sigframe sf;
  memset(&sf, 0, sizeof(sf));
  sf.magic = B1NIX_SIGFRAME_MAGIC;
  if (task_has_saved_sigmask(t)) {
    sf.old_blocked_signals = task_saved_sigmask(t);
    task_clear_saved_sigmask(t);
  } else {
    sf.old_blocked_signals = t->blocked_signals;
  }
  sf.saved_frame = *frame;

  if (syscall_copyout((void *)(usize)frame_base, &sf, sizeof(sf)) < 0 ||
      syscall_copyout((void *)(usize)restorer_slot, &restorer,
                      sizeof(restorer)) < 0) {
    console_write("signal: failed to build user frame\n");
    scheduler_exit_current(-SIGSEGV);
    return;
  }

  if (is_siginfo) {
    /* `frame` still holds the interrupted user context here (the handler regs
     * are set further down), so the ucontext captures the correct register
     * snapshot. */
    struct linux_siginfo si;
    memset(&si, 0, sizeof(si));
    si.si_signo = b1nix_signo_to_linux(sig);
    if (!si.si_signo)
      si.si_signo = sig;
    si.si_code = si_code; /* SI_USER(0) / SI_QUEUE(-1) / SI_TIMER(-2) */
    /* Carry the queued payload so an SA_SIGINFO handler reads si_value —
     * required by sigqueue(3) and SIGEV_SIGNAL POSIX timers (M74). */
    si.si_value = (long)(usize)si_val.sival_ptr;

    struct linux_ucontext uc;
    memset(&uc, 0, sizeof(uc));
    uc.gregs[LX_REG_R8] = frame->r8;
    uc.gregs[LX_REG_R9] = frame->r9;
    uc.gregs[LX_REG_R10] = frame->r10;
    uc.gregs[LX_REG_R11] = frame->r11;
    uc.gregs[LX_REG_R12] = frame->r12;
    uc.gregs[LX_REG_R13] = frame->r13;
    uc.gregs[LX_REG_R14] = frame->r14;
    uc.gregs[LX_REG_R15] = frame->r15;
    uc.gregs[LX_REG_RDI] = frame->rdi;
    uc.gregs[LX_REG_RSI] = frame->rsi;
    uc.gregs[LX_REG_RBP] = frame->rbp;
    uc.gregs[LX_REG_RBX] = frame->rbx;
    uc.gregs[LX_REG_RDX] = frame->rdx;
    uc.gregs[LX_REG_RAX] = frame->rax;
    uc.gregs[LX_REG_RCX] = frame->rcx;
    uc.gregs[LX_REG_RSP] = frame->rsp;
    uc.gregs[LX_REG_RIP] = frame->rip;
    uc.gregs[LX_REG_EFL] = frame->rflags;
    uc.gregs[LX_REG_CSGSFS] = frame->cs;
    uc.gregs[LX_REG_ERR] = frame->error_code;
    uc.gregs[LX_REG_TRAPNO] = frame->vector;
    uc.uc_sigmask = b1nix_sigset_to_linux(sf.old_blocked_signals);

    if (syscall_copyout((void *)(usize)si_addr, &si, sizeof(si)) < 0 ||
        syscall_copyout((void *)(usize)uc_addr, &uc, sizeof(uc)) < 0) {
      console_write("signal: failed to build siginfo/ucontext\n");
      scheduler_exit_current(-SIGSEGV);
      return;
    }
  } else if (is_native_siginfo) {
    /* Native SA_SIGINFO: hand the handler a native siginfo_t carrying the RT
     * payload in si_value. ucontext (RDX) is null — the payload of interest is
     * the value, not the saved register set. */
    struct b1nix_native_siginfo si;
    memset(&si, 0, sizeof(si));
    si.si_signo = sig;
    si.si_code = si_code;
    si.si_value.sival_ptr = si_val.sival_ptr;
    if (syscall_copyout((void *)(usize)si_addr, &si, sizeof(si)) < 0) {
      console_write("signal: failed to build native siginfo\n");
      scheduler_exit_current(-SIGSEGV);
      return;
    }
  }

  /* Block mask for handler execution. */
  t->blocked_signals |= sa->sa_mask;
  if (!(sa->sa_flags & SA_NODEFER))
    t->blocked_signals |= (1ULL << (sig - 1));

  frame->rip = (u64)(usize)sa->sa_handler;
  frame->rsp = restorer_slot;
  /* A Linux-personality handler expects the Linux signal number, not b1nix's. */
  if (img && img->personality == PERSONALITY_LINUX) {
    int lx = b1nix_signo_to_linux(sig);
    frame->rdi = (u64)(lx ? lx : sig);
  } else {
    frame->rdi = (u64)sig;
  }
  if (is_siginfo) {
    frame->rsi = si_addr; /* siginfo_t * */
    frame->rdx = uc_addr; /* ucontext_t * */
  } else if (is_native_siginfo) {
    frame->rsi = si_addr; /* native siginfo_t * */
    frame->rdx = 0;       /* ucontext_t * (not provided) */
  }
  frame->vector = 0; /* Force return via iretq to honor the modified rip */
  /* Note: do NOT update saved_user_rsp here — it already holds the original
   * user RSP and will be refreshed by the SYSCALL entry on the next entry.
   * Updating it with restorer_slot (the modified RSP) would be wrong. */
}

void arch_check_and_deliver_signals(struct interrupt_frame *frame) {
  /* rseq(2): this is a return to ring 3 after something that could have
   * preempted or migrated the task, which is exactly when the registered
   * cpu_id must be refreshed and an interrupted critical section aborted. */
  if (frame && (frame->cs == 0x1B || frame->cs == 0x23))
    rseq_on_return_to_user(frame);

  if (!current_task) return;

    /* Block signals during delivery check to prevent reentrancy issues */
    interrupts_disable();

    /* Acquire-load: another CPU's scheduler_kill sets bits with a release
     * fetch_or. blocked_signals is task-local. */
    u64 pending = __atomic_load_n(&current_task->pending_signals,
                                  __ATOMIC_ACQUIRE) & ~current_task->blocked_signals;

    if (pending == 0) {
        interrupts_enable();
        return;
    }

    if (klog_debug_enabled("signal")) {
      char sigbuf[192];
      snprintf(sigbuf, sizeof(sigbuf),
               "pending task=%s pid=%u mask=%p rip=%p rsp=%p",
               current_task->name ? current_task->name : "?",
               (unsigned)current_task->id, (void *)(usize)pending,
               (void *)(usize)frame->rip, (void *)(usize)frame->rsp);
      klog_debug_category("signal", sigbuf);
    }

    for (int i = 1; i <= NSIG; i++) {
        if (pending & (1ULL << (i - 1))) {
            /* ptrace(2): a traced task stops here instead of acting on the
             * signal, and its tracer decides whether the signal is delivered,
             * replaced or swallowed. */
            if (ptrace_is_traced(current_task)) {
                __atomic_fetch_and(&current_task->pending_signals,
                                   ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
                interrupts_enable();
                int consumed = ptrace_signal_stop(current_task, i, frame);
                if (consumed)
                    return;
                interrupts_disable();
            }
            struct sigaction *sa = &current_task->sigactions[i - 1];
            if (klog_debug_enabled("signal")) {
              char sigbuf[160];
              snprintf(sigbuf, sizeof(sigbuf),
                       "action task=%s pid=%u sig=%d handler=%p flags=%p pending=%p",
                       current_task->name ? current_task->name : "?",
                       (unsigned)current_task->id, i, (void *)(usize)sa->sa_handler,
                       (void *)(usize)sa->sa_flags, (void *)(usize)pending);
              klog_debug_category("signal", sigbuf);
            }
            if (sa->sa_handler != SIG_IGN && sa->sa_handler != SIG_DFL) {
                /* Deliver signal: build frame and redirect execution. Standard
                 * signals carry no RT payload (SI_USER, zero value). */
                arch_build_signal_frame(frame, i, B1NIX_SI_USER,
                                        (union sigval){.sival_ptr = 0});

                /* Clear pending bit */
                __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)), __ATOMIC_RELAXED);

                interrupts_enable();
                return; /* Deliver one signal at a time */
            } else if (sa->sa_handler == SIG_DFL) {
                /* Default actions: most kill the process */
                if (i == SIGCHLD || i == SIGURG || i == SIGWINCH) {
                    __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
                } else if (i == SIGCONT) {
                    __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
                    if (current_task->state == TASK_STOPPED) {
                        current_task->state = TASK_READY;
                        current_task->continued_report_pending = 1;
                        scheduler_notify_wait_event(current_task->parent_id);
                    }
                } else if (i == SIGSTOP || i == SIGTSTP ||
                           i == SIGTTIN || i == SIGTTOU) {
                    current_task->state = TASK_STOPPED;
                    current_task->last_stop_signal = i;
                    current_task->stop_report_pending = 1;
                    __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
                    scheduler_notify_wait_event(current_task->parent_id);
                    interrupts_enable();
                    scheduler_yield();
                    return;
                } else if (scheduler_get_init_pid() &&
                           current_task->id == scheduler_get_init_pid()) {
                    /* PID 1 ignores signals it has no handler for — see the
                     * matching guard in scheduler_deliver_pending_signals. */
                    __atomic_fetch_and(&current_task->pending_signals,
                                       ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
                } else {
                    /* Only log genuine fault signals (crashes worth
                     * diagnosing). Intentional kills — SIGKILL/SIGTERM/etc,
                     * e.g. the sibling threads exit_group() terminates — are
                     * expected termination; logging them just races userspace
                     * serial output and corrupts adjacent test markers. */
                    if (i == SIGSEGV || i == SIGILL || i == SIGBUS ||
                        i == SIGFPE || i == SIGABRT) {
                        console_write("signal: process pid=");
                        console_write_dec(current_task->id);
                        console_write(" killed by signal ");
                        console_write_dec(i);
                        console_write(" rip=");
                        console_write_hex64(frame->rip);
                        console_write("\n");
                    }
                    /* Encode "killed by signal i" as 128+i so waitpid reports
                     * WIFSIGNALED with WTERMSIG == i (see scheduler_waitpid). */
                    scheduler_exit_current(128 + i);
                }
            } else {
                /* SIG_IGN: discard the signal so its pending bit doesn't
                 * linger and get re-examined on every delivery check. */
                __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
            }
        }
    }

    /* M74: RT signals (SIGRTMIN..SIGRTMAX), delivered after the standard 1..NSIG
     * signals. Each is QUEUED — one delivery dequeues one instance (FIFO, lowest
     * signo first); the pending bit clears only when that signo's queue drains,
     * so N sends yield N deliveries (no coalescing). */
    for (int i = SIGRTMIN; i <= SIGRTMAX; i++) {
        if (!(pending & (1ULL << (i - 1))))
            continue;
        struct sigaction *sa = scheduler_rt_action_current(i);
        int more = 0, code = 0;
        union sigval val;
        val.sival_ptr = 0;
        if (sa && sa->sa_handler != SIG_IGN && sa->sa_handler != SIG_DFL) {
            /* scheduler_rt_dequeue_current clears the pending bit itself (under
             * g_rt_lock) when this signo's queue drains — see the signal-loss
             * race note there. Only deliver if an instance was actually dequeued. */
            if (scheduler_rt_dequeue_current(i, &code, &val, &more)) {
                arch_build_signal_frame(frame, i, code, val);
                interrupts_enable();
                return; /* one signal per delivery check */
            }
            /* Bit set with no queued entry should not happen (enqueue sets the
             * bit and adds the entry atomically); clear a stale bit defensively. */
            __atomic_fetch_and(&current_task->pending_signals,
                               ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
        } else if (!sa || sa->sa_handler == SIG_DFL) {
            /* RT default action is terminate the process. */
            console_write("signal: process pid=");
            console_write_dec(current_task->id);
            console_write(" killed by RT signal ");
            console_write_dec(i);
            console_write("\n");
            scheduler_exit_current(TASK_EXIT_SIGNALED | i);
        } else {
            /* SIG_IGN: discard one queued instance (the dequeue clears the bit
             * when the queue drains). */
            scheduler_rt_dequeue_current(i, &code, &val, &more);
        }
    }

    interrupts_enable();
}

u64 sys_sigreturn(struct interrupt_frame *frame) {
  struct task *t = current_task;

  u64 sp = frame->rsp;
  u64 sf_addr = sp;
  struct b1nix_sigframe sf;

  if (syscall_copyin(&sf, (void *)(usize)sf_addr, sizeof(sf)) < 0) {
    scheduler_exit_current(-SIGSEGV);
  }

  if (sf.magic != B1NIX_SIGFRAME_MAGIC) {
    return (u64)-EINVAL;
  }

  /* Privilege checks: user cannot forge kernel return state. */
  if (sf.saved_frame.cs != 0x23 || sf.saved_frame.ss != 0x1B) {
    return (u64)-EINVAL;
  }
  if (sf.saved_frame.rip >= 0x0000800000000000ULL ||
      sf.saved_frame.rsp >= 0x0000800000000000ULL) {
    return (u64)-EINVAL;
  }

  /* Preserve IF and keep user-modifiable status bits conservative. */
  sf.saved_frame.rflags &= 0x00000000003f7fd7ULL;
  sf.saved_frame.rflags |= 0x200ULL;

  t->blocked_signals = sf.old_blocked_signals;
  memcpy(frame, &sf.saved_frame, sizeof(*frame));
  t->saved_user_rsp = frame->rsp;

  return frame->rax;
}
