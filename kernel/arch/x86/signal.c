#include <b1nix/arch_x86.h>
#include <b1nix/sched.h>
#include <b1nix/signal.h>
#include <b1nix/syscall.h>
#include <b1nix/arch.h>
#include <b1nix/klog.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <string.h>


static int is_valid_user_code_ptr(u64 ptr) {
  if (ptr == 0)
    return 0;
  return ptr < 0x0000800000000000ULL;
}

static void arch_build_signal_frame(struct interrupt_frame *frame, int sig) {
  struct task *t = current_task;
  struct sigaction *sa = &t->sigactions[sig - 1];

  /* Preserve x86_64 SysV red zone (128 bytes below RSP). */
  u64 user_rsp = frame->rsp - 128;
  u64 frame_base = (user_rsp - sizeof(struct b1nix_sigframe)) & ~0xFULL;
  u64 restorer_slot = frame_base - sizeof(u64);

  if (!is_valid_user_code_ptr((u64)(usize)sa->sa_handler) ||
      !is_valid_user_code_ptr((u64)(usize)sa->sa_restorer)) {
    scheduler_exit_current(-SIGSEGV);
    return;
  }

  struct b1nix_sigframe sf;
  memset(&sf, 0, sizeof(sf));
  sf.magic = B1NIX_SIGFRAME_MAGIC;
  sf.old_blocked_signals = t->blocked_signals;
  sf.saved_frame = *frame;

  if (syscall_copyout((void *)(usize)frame_base, &sf, sizeof(sf)) < 0 ||
      syscall_copyout((void *)(usize)restorer_slot, &sa->sa_restorer,
                      sizeof(sa->sa_restorer)) < 0) {
    console_write("signal: failed to build user frame\n");
    scheduler_exit_current(-SIGSEGV);
    return;
  }

  /* Block mask for handler execution. */
  t->blocked_signals |= sa->sa_mask;
  if (!(sa->sa_flags & SA_NODEFER))
    t->blocked_signals |= (1ULL << (sig - 1));

  frame->rip = (u64)(usize)sa->sa_handler;
  frame->rsp = restorer_slot;
  frame->rdi = (u64)sig;
  /* Note: do NOT update saved_user_rsp here — it already holds the original
   * user RSP and will be refreshed by the SYSCALL entry on the next entry.
   * Updating it with restorer_slot (the modified RSP) would be wrong. */
}

void arch_check_and_deliver_signals(struct interrupt_frame *frame) {
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

    for (int i = 1; i <= NSIG; i++) {
        if (pending & (1ULL << (i - 1))) {
            struct sigaction *sa = &current_task->sigactions[i - 1];
            if (sa->sa_handler != SIG_IGN && sa->sa_handler != SIG_DFL) {
                /* Deliver signal: build frame and redirect execution */
                arch_build_signal_frame(frame, i);

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
                } else {
                    console_write("signal: process killed by signal\n");
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
  if (sf.saved_frame.cs != 0x1B || sf.saved_frame.ss != 0x23) {
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
