#include <b1nix/arch_x86.h>
#include <b1nix/sched.h>
#include <b1nix/signal.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/arch.h>
#include <b1nix/klog.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <string.h>

static int is_valid_user_code_ptr(u64 ptr) {
  if (ptr == 0)
    return 0;
  return ptr < USER_SPACE_LIMIT;
}

static void arch_build_signal_frame(struct interrupt_frame *frame, int sig) {
  struct task *t = current_task;
  struct sigaction *sa = &t->sigactions[sig - 1];

  /* 32-bit: no red zone. */
  u32 user_esp = frame->esp;
  /* SA_ONSTACK: deliver onto the alternate signal stack when one is registered
   * and we are not already running on it (POSIX: do not nest onto it). */
  if ((sa->sa_flags & SA_ONSTACK) && !task_on_altstack(t, frame->esp)) {
    u64 alt_top = task_altstack_top(t);
    if (alt_top)
      user_esp = (u32)alt_top;
  }
  u32 frame_base = (user_esp - sizeof(struct b1nix_sigframe)) & ~0xF;
  u32 restorer_slot = frame_base - 8;

  /* Prefer the kernel-owned signal-return trampoline (mapped RO+exec at exec);
   * fall back to a userspace-supplied sa_restorer only if it was not mapped
   * (e.g. OOM at exec). The trampoline is tamper-proof — userspace cannot
   * redirect the return path, which matters for setuid programs. */
  struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
  u64 restorer = (img && img->sigreturn_trampoline)
                     ? img->sigreturn_trampoline
                     : (u64)(usize)sa->sa_restorer;
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

  u32 sig_arg = (u32)sig;
  u32 restorer_val = (u32)restorer;

  if (syscall_copyout((void *)(usize)frame_base, &sf, sizeof(sf)) < 0 ||
      syscall_copyout((void *)(usize)(frame_base - 4), &sig_arg, sizeof(sig_arg)) < 0 ||
      syscall_copyout((void *)(usize)restorer_slot, &restorer_val, sizeof(restorer_val)) < 0) {
    console_write("signal: failed to build user frame\n");
    scheduler_exit_current(-SIGSEGV);
    return;
  }

  /* Block mask for handler execution. */
  t->blocked_signals |= sa->sa_mask;
  if (!(sa->sa_flags & SA_NODEFER))
    t->blocked_signals |= (1ULL << (sig - 1));

  frame->eip = (u32)(usize)sa->sa_handler;
  frame->esp = restorer_slot;
}

void arch_check_and_deliver_signals(struct interrupt_frame *frame) {
  if (!current_task) return;

  interrupts_disable();

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
        arch_build_signal_frame(frame, i);
        __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
        interrupts_enable();
        return;
      } else if (sa->sa_handler == SIG_DFL) {
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
          scheduler_exit_current(128 + i);
        }
      } else {
        __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
      }
    }
  }

  interrupts_enable();
}

u64 sys_sigreturn(struct interrupt_frame *frame) {
  struct task *t = current_task;
  /* On 32-bit: esp is at restorer_slot + 4 = frame_base - 4.
   * The sigframe is at frame_base = esp + 4. */
  u32 sf_addr = frame->esp + 4;
  struct b1nix_sigframe sf;

  if (syscall_copyin(&sf, (void *)(usize)sf_addr, sizeof(sf)) < 0) {
    scheduler_exit_current(-SIGSEGV);
  }

  if (sf.magic != B1NIX_SIGFRAME_MAGIC) {
    return (u64)-EINVAL;
  }

  /* Privilege checks */
  if (sf.saved_frame.cs != 0x1B || sf.saved_frame.ss != 0x23) {
    return (u64)-EINVAL;
  }
  if (sf.saved_frame.eip >= USER_SPACE_LIMIT ||
      sf.saved_frame.esp >= USER_SPACE_LIMIT) {
    return (u64)-EINVAL;
  }

  sf.saved_frame.eflags &= 0x003f7fd7ULL;
  sf.saved_frame.eflags |= 0x200ULL;

  t->blocked_signals = sf.old_blocked_signals;
  memcpy(frame, &sf.saved_frame, sizeof(*frame));
  t->saved_user_rsp = frame->esp;

  return frame->eax;
}
