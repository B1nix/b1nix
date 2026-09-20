/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/arch_x86_64.h>
#include <b1nix/linux_abi.h>
#include <b1nix/pkeys.h>
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
  /* SA_SIGINFO: the handler is the 3-arg form and needs a Linux siginfo_t in
   * RSI and a ucontext_t in RDX. Both are placed ABOVE the sigframe so the
   * handler's downward stack growth never clobbers them. */
  int is_siginfo = img && (sa->sa_flags & SA_SIGINFO);
  u64 si_addr = 0, uc_addr = 0;
  u64 top = user_rsp;
  if (is_siginfo) {
    si_addr = (top - sizeof(struct linux_siginfo)) & ~0xFULL;
    uc_addr = (si_addr - sizeof(struct linux_ucontext)) & ~0xFULL;
    top = uc_addr;
  }
  /* The interrupted FPU state, above the frame: an XSAVE image needs 64-byte
   * alignment, and the registers are still the interrupted code's here (the
   * kernel never touches them). Saved into the task's own area first, which
   * doubles as the copy the scheduler keeps, then copied out. */
  void *xarea = task_fpu_alloc(t) ? task_xsave_area(t) : 0;
  u64 fpu_size = xarea ? (u64)arch_xsave_area_size() : 512;
  u64 fpu_addr = (top - fpu_size) & ~0x3FULL;
  top = fpu_addr;
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
  sf.pkru = arch_pkru_user_get();
  if (task_has_saved_sigmask(t)) {
    sf.old_blocked_signals = task_saved_sigmask(t);
    task_clear_saved_sigmask(t);
  } else {
    sf.old_blocked_signals = t->blocked_signals;
  }
  sf.saved_frame = *frame;
  sf.fpu_addr = fpu_addr;
  sf.fpu_size = fpu_size;
  {
    const void *fpu_src;

    if (xarea) {
      arch_xsave(xarea, arch_xsave_mask());
      fpu_src = xarea;
    } else {
      arch_fpu_save(t->fpu_state);
      fpu_src = t->fpu_state;
    }
    if (syscall_copyout((void *)(usize)fpu_addr, fpu_src, (usize)fpu_size) < 0) {
      console_write("signal: failed to save the FPU state on the user stack\n");
      scheduler_exit_current(-SIGSEGV);
      return;
    }
  }

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
    /* M80: for a signal a CPU fault produced, the handler wants the faulting
     * address. In Linux's siginfo_t the _sigfault._addr member aliases
     * si_pid/si_uid at offset 16 — a fault signal has no sending pid, so the
     * union's fault arm is the correct one to fill here. This is what lets a
     * crash reporter record the bad address from inside its own handler. */
    {
      int fsig = 0, fcode = 0;
      u64 faddr = 0;
      if (ptrace_fault_info(t, &fsig, &faddr, &fcode) && fsig == sig) {
        si.si_code = fcode;
        memcpy((u8 *)&si + 16, &faddr, sizeof(faddr));
        /* _sigfault._addr_pkey._pkey: after the address and 8 bytes of the
         * union's padding. */
        if (fcode == B1NIX_SEGV_PKUERR) {
          u32 pk = ptrace_fault_pkey(t);
          memcpy((u8 *)&si + 32, &pk, sizeof(pk));
        }
      }
    }

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
  }

  /* Block mask for handler execution. */
  t->blocked_signals |= sa->sa_mask;
  if (!(sa->sa_flags & SA_NODEFER))
    t->blocked_signals |= (1ULL << (sig - 1));

  frame->rip = (u64)(usize)sa->sa_handler;
  frame->rsp = restorer_slot;
  /* A user handler expects the Linux signal number, not b1nix's. */
  if (img) {
    int lx = b1nix_signo_to_linux(sig);
    frame->rdi = (u64)(lx ? lx : sig);
  } else {
    frame->rdi = (u64)sig;
  }
  if (is_siginfo) {
    frame->rsi = si_addr; /* siginfo_t * */
    frame->rdx = uc_addr; /* ucontext_t * */
  }
  frame->vector = 0; /* Force return via iretq to honor the modified rip */
  /* The handler starts from the initial key rights, whatever the interrupted
   * code had (Linux resets PKRU on signal delivery); sigreturn restores them. */
  if (arch_pkeys_enabled())
    arch_pkeys_signal_rights();
  /* Note: do NOT update saved_user_rsp here — it already holds the original
   * user RSP and will be refreshed by the SYSCALL entry on the next entry.
   * Updating it with restorer_slot (the modified RSP) would be wrong. */
}

/* The mask sigsuspend installed stays installed until a handler has actually
 * been entered.
 *
 * sigsuspend(2) waits with a caller-supplied mask and must run the handler for
 * the signal that ends the wait WITH that mask still in force; only the
 * handler's own return puts the original mask back, which the signal frame
 * carries. Restoring it in the syscall itself — as this kernel did — means the
 * signal that just ended the wait is blocked again the instant the wait ends,
 * so no handler ever runs, the pending bit is never consumed, and the program
 * calls sigsuspend again with the same signal still pending.
 *
 * That is not a slow path, it is a livelock: a shell waiting for a child it had
 * already reaped repeated rt_sigsuspend forever, and with it went every script
 * driving the machine. The wait ends here instead — after delivery has had its
 * chance — and only when nothing was delivered, which is the case Linux calls
 * restore_saved_sigmask(). */
static void arch_deliver_signals_body(struct interrupt_frame *frame);

void arch_check_and_deliver_signals(struct interrupt_frame *frame) {
  arch_deliver_signals_body(frame);
  /* Every return to user mode passes here: put back key rights a kernel copy
   * had to clear (arch_pkru_kernel_fault). */
  if (frame && (frame->cs == 0x1B || frame->cs == 0x23))
    arch_pkru_return_to_user();

  /* Returning to ring 3 only: a kernel-mode return can land here while the task
   * is still parked inside sigsuspend itself, and the temporary mask has to
   * survive that. */
  if (!current_task || !frame)
    return;
  if (frame->cs != 0x1B && frame->cs != 0x23)
    return;
  if (task_has_saved_sigmask(current_task)) {
    current_task->blocked_signals = task_saved_sigmask(current_task);
    task_clear_saved_sigmask(current_task);
  }
}

static void arch_deliver_signals_body(struct interrupt_frame *frame) {
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

    for (int i = 1; i < NSIG; i++) {
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
                    scheduler_self_stop(i);
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
                    /* "Killed by signal i" is a FLAG BIT, not a value range:
                     * scheduler_waitpid reads TASK_EXIT_SIGNALED, because a
                     * program that calls exit(137) must not be mistaken for one
                     * killed by signal 9. This site still passed 128+i long
                     * after the encoding changed, so every default-action kill
                     * reported a normal exit — WIFSIGNALED was false for a
                     * process the kernel had just killed. The RT branch below
                     * has always had it right. */
                    scheduler_exit_current(TASK_EXIT_SIGNALED | i);
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
  if (arch_pkeys_enabled())
    arch_pkru_user_set((u32)sf.pkru);

  /* The FPU state the handler interrupted, from the user stack. The image is
   * user memory by now, so it is sanitised the way the hardware demands
   * before xrstor/fxrstor see it: reserved MXCSR bits clear, XSTATE_BV within
   * the enabled mask, XCOMP_BV and the header's reserved words zero. A frame
   * without one (fpu_size 0) is left alone. */
  if (sf.fpu_size && sf.fpu_addr < 0x0000800000000000ULL) {
    void *xarea = task_xsave_area(t);
    usize want = xarea ? arch_xsave_area_size() : 512;

    if (sf.fpu_size != want)
      return (u64)-EINVAL;
    u8 *dst = xarea ? (u8 *)xarea : (u8 *)t->fpu_state;
    if (syscall_copyin(dst, (void *)(usize)sf.fpu_addr, want) < 0)
      return (u64)-EFAULT;
    u32 mxcsr;
    memcpy(&mxcsr, dst + 24, sizeof(mxcsr));
    mxcsr &= 0xFFFFu;
    memcpy(dst + 24, &mxcsr, sizeof(mxcsr));
    if (xarea) {
      u64 bv;
      memcpy(&bv, dst + 512, sizeof(bv));
      bv &= arch_xsave_mask();
      memcpy(dst + 512, &bv, sizeof(bv));
      memset(dst + 520, 0, 64 - 8);
      arch_xrstor(xarea, arch_xsave_mask());
    } else {
      arch_fpu_restore(t->fpu_state);
    }
  }
  memcpy(frame, &sf.saved_frame, sizeof(*frame));
  t->saved_user_rsp = frame->rsp;

  return frame->rax;
}
