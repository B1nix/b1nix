#include <b1nix/arch_x86.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/arch.h>
#include <b1nix/klog.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <string.h>

extern struct task *current_task;

static void arch_build_signal_frame(struct interrupt_frame *frame, int sig) {
    struct task *t = current_task;
    struct sigaction *sa = &t->sigactions[sig - 1]; // sig 1 is index 0

    u64 sp = frame->rsp;

    /* Ensure 16-byte alignment before pushing anything */
    sp &= ~0xf;

    /* Space for saved context (struct interrupt_frame) */
    sp -= sizeof(struct interrupt_frame);

    /* Copy current kernel state (interrupt_frame) to user stack */
    if (syscall_copyout((void*)sp, frame, sizeof(struct interrupt_frame)) < 0) {
        console_write("signal: failed to push context to user stack\n");
        scheduler_exit_current(-SIGSEGV);
        return;
    }

    /* Push saved blocked_signals mask */
    sp -= 8;
    if (syscall_copyout((void*)sp, &t->blocked_signals, 8) < 0) {
        console_write("signal: failed to push blocked_signals to user stack\n");
        scheduler_exit_current(-SIGSEGV);
        return;
    }

    /* Push restorer address (trampoline) */
    sp -= 8;
    if (syscall_copyout((void*)sp, &sa->sa_restorer, 8) < 0) {
        console_write("signal: failed to push restorer to user stack\n");
        scheduler_exit_current(-SIGSEGV);
        return;
    }

    /* Update frame for return to handler */
    frame->rsp = sp;
    frame->rip = (u64)sa->sa_handler;
    frame->rdi = (u64)sig;

    /* If this was called from a syscall, we must also update task->saved_user_rsp
     * because x86_syscall_return restores RSP from there. */
    if (t->saved_user_rsp != 0) {
        t->saved_user_rsp = sp;
    }
}

void arch_check_and_deliver_signals(struct interrupt_frame *frame) {
    if (!current_task) return;

    /* Block signals during delivery check to prevent reentrancy issues */
    interrupts_disable();

    u64 pending = current_task->pending_signals & ~current_task->blocked_signals;
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
                current_task->pending_signals &= ~(1ULL << (i - 1));

                /* If not SA_NODEFER, block this signal during its handler */
                if (!(sa->sa_flags & SA_NODEFER)) {
                    current_task->blocked_signals |= (1ULL << (i - 1));
                }

                interrupts_enable();
                return; /* Deliver one signal at a time */
            } else if (sa->sa_handler == SIG_DFL) {
                /* Default actions: most kill the process */
                if (i == SIGCHLD || i == SIGURG || i == SIGWINCH) {
                    current_task->pending_signals &= ~(1ULL << (i - 1));
                } else {
                    console_write("signal: process killed by signal\n");
                    scheduler_exit_current(-i);
                }
            }
        }
    }

    interrupts_enable();
}

u64 sys_sigreturn(struct interrupt_frame *frame) {
    struct task *t = current_task;
    u64 sp = frame->rsp;

    /* Frame was pushed to user stack:
     * [restorer]        <--- SP
     * [blocked_signals] <--- SP + 8
     * [interrupt_frame] <--- SP + 16
     *
     * The handler returns with `ret`, which pops [restorer]. The restorer then
     * enters the kernel through `syscall`, so the saved user RSP points at
     * [blocked_signals], not at [restorer].
     */

    u64 blocked_signals_ptr = sp;
    u64 context_ptr = sp + 8;

    u64 old_blocked;
    if (syscall_copyin(&old_blocked, (void*)blocked_signals_ptr, 8) < 0) {
        scheduler_exit_current(-SIGSEGV);
    }
    t->blocked_signals = old_blocked;

    struct interrupt_frame kframe;
    if (syscall_copyin(&kframe, (void*)context_ptr, sizeof(kframe)) < 0) {
        scheduler_exit_current(-SIGSEGV);
    }

    /* Restore register state into current kernel frame */
    memcpy(frame, &kframe, sizeof(struct interrupt_frame));

    /* Update saved_user_rsp since it might have changed */
    t->saved_user_rsp = frame->rsp;

    return frame->rax;
}
