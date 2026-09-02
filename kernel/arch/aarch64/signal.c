/* aarch64 signal delivery.
 *
 * Same model as kernel/arch/x86_64/signal.c — a struct b1nix_sigframe is
 * pushed on the interrupted task's own user stack, the register frame is
 * rewritten to enter the handler, and sigreturn(2) restores the frame — with
 * the AAPCS64 differences that matter:
 *   - no red zone below SP,
 *   - the handler's return address lives in x30 (lr), not on the stack, so
 *     the sigreturn trampoline address goes there instead of a pushed slot,
 *   - handler args are x0 (signo), x1 (siginfo *), x2 (ucontext *).
 */
#include <b1nix/arch.h>
#include <b1nix/arch_aarch64.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/linux_abi.h>
#include <b1nix/sched.h>
#include <b1nix/signal.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/ptrace.h>
#include <string.h>

/* EL0 virtual addresses live in the TTBR0 half; anything with the top bits set
 * is a kernel address a user frame must never name. */
#define USER_ADDR_LIMIT 0x0001000000000000ULL

static int is_valid_user_ptr(u64 ptr) {
  return ptr != 0 && ptr < USER_ADDR_LIMIT;
}

static void arch_build_signal_frame(struct interrupt_frame *frame, int sig,
                                    int si_code, union sigval si_val) {
  struct task *t = current_task;
  struct sigaction *sa = SIG_IS_RT(sig) ? scheduler_rt_action_current(sig)
                                        : &t->sigactions[sig - 1];
  if (!sa) {
    scheduler_exit_current(-SIGSEGV);
    return;
  }

  u64 user_sp = frame->sp_el0;
  if ((sa->sa_flags & SA_ONSTACK) && !task_on_altstack(t, frame->sp_el0)) {
    u64 alt_top = task_altstack_top(t);
    if (alt_top)
      user_sp = alt_top;
  }

  struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
  /* SA_SIGINFO: hand the handler a siginfo_t. The third argument (ucontext)
   * is left null — nothing in the ported userspace reads uc_mcontext on this
   * arch yet, and a half-filled one would be worse than an obvious null.
   * ponytail: add the aarch64 sigcontext layout when something needs it. */
  int is_siginfo = (sa->sa_flags & SA_SIGINFO) != 0;
  int is_linux = img && img->personality == PERSONALITY_LINUX;
  u64 si_addr = 0;
  u64 uc_addr = 0;
  u64 top = user_sp & ~0xFULL;
  if (is_siginfo) {
    usize si_size = is_linux ? sizeof(struct linux_siginfo)
                             : sizeof(struct b1nix_native_siginfo);
    si_addr = (top - si_size) & ~0xFULL;
    top = si_addr;
  }
  /* Linux-personality SA_SIGINFO handlers get a real ucontext_t as their third
   * argument. It used to be null here, and a null is what Crashpad recorded as
   * the crashing thread's context — its handler then read the register set from
   * address 0, got EIO, and wrote no minidump at all. Like the x86_64 side,
   * this snapshot is informational: sigreturn restores from b1nix's own frame,
   * so a handler that edits uc_mcontext is not obeyed yet. */
  if (is_siginfo && is_linux) {
    uc_addr = (top - sizeof(struct linux_ucontext_aarch64)) & ~0xFULL;
    top = uc_addr;
  }
  u64 frame_base = (top - sizeof(struct b1nix_sigframe)) & ~0xFULL;

  u64 restorer = (sa->sa_restorer) ? (u64)(usize)sa->sa_restorer
                                   : (img ? img->sigreturn_trampoline : 0);
  if (!is_valid_user_ptr((u64)(usize)sa->sa_handler) ||
      !is_valid_user_ptr(restorer)) {
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

  if (syscall_copyout((void *)(usize)frame_base, &sf, sizeof(sf)) < 0) {
    console_write("signal: failed to build user frame\n");
    scheduler_exit_current(-SIGSEGV);
    return;
  }

  if (is_siginfo && is_linux) {
    struct linux_siginfo si;
    memset(&si, 0, sizeof(si));
    si.si_signo = b1nix_signo_to_linux(sig);
    if (!si.si_signo)
      si.si_signo = sig;
    si.si_code = si_code;
    si.si_value = (long)(usize)si_val.sival_ptr;
    {
      int fsig = 0, fcode = 0;
      u64 faddr = 0;
      if (ptrace_fault_info(t, &fsig, &faddr, &fcode) && fsig == sig) {
        si.si_code = fcode;
        memcpy((u8 *)&si + 16, &faddr, sizeof(faddr));
      }
    }
    if (syscall_copyout((void *)(usize)si_addr, &si, sizeof(si)) < 0) {
      console_write("signal: failed to build siginfo\n");
      scheduler_exit_current(-SIGSEGV);
      return;
    }

    /* `frame` still holds the interrupted user context here — the handler's own
     * registers are installed further down — so this captures the right
     * snapshot. */
    struct linux_ucontext_aarch64 *uc = kzalloc(sizeof(*uc));
    if (!uc) {
      console_write("signal: out of memory building ucontext\n");
      scheduler_exit_current(-SIGSEGV);
      return;
    }
    uc->uc_sigmask = b1nix_sigset_to_linux(sf.old_blocked_signals);
    {
      int fsig = 0, fcode = 0;
      u64 faddr = 0;
      if (ptrace_fault_info(t, &fsig, &faddr, &fcode) && fsig == sig)
        uc->uc_mcontext.fault_address = faddr;
    }
    /* x0-x30 are consecutive u64 fields of the frame, in register order. */
    {
      const u64 *gp = &frame->x0;
      for (int i = 0; i < 31; i++)
        uc->uc_mcontext.regs[i] = gp[i];
    }
    uc->uc_mcontext.sp = frame->sp_el0;
    uc->uc_mcontext.pc = frame->elr;
    uc->uc_mcontext.pstate = frame->spsr;
    /* The reserved tail is a chain of _aarch64_ctx records: the FP/SIMD state
     * first, then a zero-filled terminator (the memset above already left
     * one). t->fpu_state is laid out as user_fpsimd_state, so the vector half
     * copies straight across. */
    {
      struct linux_fpsimd_context fp;
      memset(&fp, 0, sizeof(fp));
      fp.head.magic = LX_FPSIMD_MAGIC;
      fp.head.size = sizeof(fp);
      memcpy(fp.vregs, t->fpu_state, sizeof(fp.vregs));
      memcpy(&fp.fpsr, t->fpu_state + 512, sizeof(fp.fpsr));
      memcpy(&fp.fpcr, t->fpu_state + 516, sizeof(fp.fpcr));
      memcpy(uc->uc_mcontext.__reserved, &fp, sizeof(fp));
    }
    int uc_ok = syscall_copyout((void *)(usize)uc_addr, uc, sizeof(*uc)) == 0;
    kfree(uc);
    if (!uc_ok) {
      console_write("signal: failed to build ucontext\n");
      scheduler_exit_current(-SIGSEGV);
      return;
    }
  } else if (is_siginfo) {
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

  t->blocked_signals |= sa->sa_mask;
  if (!(sa->sa_flags & SA_NODEFER))
    t->blocked_signals |= (1ULL << (sig - 1));

  frame->elr = (u64)(usize)sa->sa_handler;
  frame->sp_el0 = frame_base;
  frame->x30 = restorer;
  if (is_linux) {
    int lx = b1nix_signo_to_linux(sig);
    frame->x0 = (u64)(lx ? lx : sig);
  } else {
    frame->x0 = (u64)sig;
  }
  frame->x1 = si_addr;
  frame->x2 = uc_addr;
}

void arch_check_and_deliver_signals(struct interrupt_frame *frame) {
  if (!current_task || !frame)
    return;
  /* Only a frame returning to EL0 can carry a signal handler. SPSR_EL1.M[3:0]
   * == 0 means the interrupted context was EL0t (userspace). */
  if ((frame->spsr & 0xFULL) != 0)
    return;

  interrupts_disable();

  u64 pending = __atomic_load_n(&current_task->pending_signals,
                                __ATOMIC_ACQUIRE) &
                ~current_task->blocked_signals;
  if (pending == 0) {
    interrupts_enable();
    return;
  }

  for (int i = 1; i < NSIG; i++) {
    if (!(pending & (1ULL << (i - 1))))
      continue;
    if (ptrace_is_traced(current_task)) {
      __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)),
                         __ATOMIC_RELAXED);
      interrupts_enable();
      int consumed = ptrace_signal_stop(current_task, i, frame);
      if (consumed)
        return;
      interrupts_disable();
    }
    struct sigaction *sa = &current_task->sigactions[i - 1];
    if (sa->sa_handler != SIG_IGN && sa->sa_handler != SIG_DFL) {
      arch_build_signal_frame(frame, i, B1NIX_SI_USER,
                              (union sigval){.sival_ptr = 0});
      __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)),
                         __ATOMIC_RELAXED);
      interrupts_enable();
      return; /* one signal per delivery check */
    }
    if (sa->sa_handler == SIG_DFL) {
      if (i == SIGCHLD || i == SIGURG || i == SIGWINCH) {
        __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)),
                           __ATOMIC_RELAXED);
      } else if (i == SIGCONT) {
        __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)),
                           __ATOMIC_RELAXED);
        if (current_task->state == TASK_STOPPED) {
          current_task->state = TASK_READY;
          current_task->continued_report_pending = 1;
          scheduler_notify_wait_event(current_task->parent_id);
        }
      } else if (i == SIGSTOP || i == SIGTSTP || i == SIGTTIN || i == SIGTTOU) {
        current_task->state = TASK_STOPPED;
        current_task->last_stop_signal = i;
        current_task->stop_report_pending = 1;
        __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)),
                           __ATOMIC_RELAXED);
        scheduler_notify_wait_event(current_task->parent_id);
        interrupts_enable();
        scheduler_yield();
        return;
      } else if (scheduler_get_init_pid() &&
                 current_task->id == scheduler_get_init_pid()) {
        /* PID 1 ignores signals it installed no handler for. */
        __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)),
                           __ATOMIC_RELAXED);
      } else {
        if (i == SIGSEGV || i == SIGILL || i == SIGBUS || i == SIGFPE ||
            i == SIGABRT) {
          console_write("signal: process pid=");
          console_write_dec(current_task->id);
          console_write(" killed by signal ");
          console_write_dec(i);
          console_write("\n");
        }
        scheduler_exit_current(128 + i);
      }
    } else {
      /* SIG_IGN */
      __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)),
                         __ATOMIC_RELAXED);
    }
  }

  /* M74 RT signals: queued, lowest signo first, one instance per check. */
  for (int i = SIGRTMIN; i <= SIGRTMAX; i++) {
    if (!(pending & (1ULL << (i - 1))))
      continue;
    if (ptrace_is_traced(current_task)) {
      __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)),
                         __ATOMIC_RELAXED);
      interrupts_enable();
      int consumed = ptrace_signal_stop(current_task, i, frame);
      if (consumed)
        return;
      interrupts_disable();
    }
    struct sigaction *sa = scheduler_rt_action_current(i);
    int more = 0, code = 0;
    union sigval val;
    val.sival_ptr = 0;
    if (sa && sa->sa_handler != SIG_IGN && sa->sa_handler != SIG_DFL) {
      if (scheduler_rt_dequeue_current(i, &code, &val, &more)) {
        arch_build_signal_frame(frame, i, code, val);
        interrupts_enable();
        return;
      }
      __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)),
                         __ATOMIC_RELAXED);
    } else if (!sa || sa->sa_handler == SIG_DFL) {
      scheduler_exit_current(TASK_EXIT_SIGNALED | i);
    } else {
      scheduler_rt_dequeue_current(i, &code, &val, &more);
    }
  }

  interrupts_enable();
}

u64 sys_sigreturn(struct interrupt_frame *frame) {
  struct task *t = current_task;
  struct b1nix_sigframe sf;

  int rc = syscall_copyin(&sf, (void *)(usize)frame->sp_el0, sizeof(sf));
  if (rc < 0)
    scheduler_exit_current(-SIGSEGV);

  if (sf.magic != B1NIX_SIGFRAME_MAGIC)
    return (u64)-EINVAL;

  /* Userspace cannot forge a frame that returns to EL1 or jumps into kernel
   * addresses: SPSR must select EL0t, PC and SP must be user addresses. */
  if ((sf.saved_frame.spsr & 0xFULL) != 0)
    return (u64)-EINVAL;
  if (sf.saved_frame.elr >= USER_ADDR_LIMIT ||
      sf.saved_frame.sp_el0 >= USER_ADDR_LIMIT)
    return (u64)-EINVAL;
  /* Keep only the user-writable PSTATE bits (NZCV + DIT/SSBS-class flags);
   * force DAIF clear so the task cannot return with interrupts masked. */
  sf.saved_frame.spsr &= 0xF0000000ULL;

  t->blocked_signals = sf.old_blocked_signals;
  memcpy(frame, &sf.saved_frame, sizeof(*frame));

  return frame->x0;
}
