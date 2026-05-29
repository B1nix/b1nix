.section .text
.code64

.extern syscall_dispatch_impl
.extern bkl_lock
.extern bkl_unlock

.section .bss
.align 16
.global x86_syscall_stack_bottom
x86_syscall_stack_bottom:
    .skip 65536
.global x86_syscall_stack_top
x86_syscall_stack_top:

.align 8
x86_syscall_saved_user_rsp:
    .quad 0

.section .text
.code64

/* SYSCALL entry point */
.global x86_syscall_entry
.type x86_syscall_entry, @function
x86_syscall_entry:
    /* Save RAX temporarily in our scratch area */
    movq %rax, x86_syscall_saved_user_rsp(%rip)

    /* Get the per-CPU current task: GS base points at this CPU's struct percpu,
     * whose cur_task member is at offset 0x10 (see struct percpu in lapic.h;
     * a _Static_assert in scheduler.c pins the offset). The kernel never uses
     * SWAPGS, so GS already holds the kernel per-CPU base in the syscall path. */
    movq %gs:0x10, %rax

    /* Save user RSP to task->saved_user_rsp (offset 112) */
    movq %rsp, 112(%rax)

    /* Load kernel RSP from task->kernel_stack_ptr (offset 104) */
    movq 104(%rax), %rsp

    /* Build a full struct interrupt_frame on the kernel stack */
    /* Order (highest address to lowest): SS, RSP, RFLAGS, CS, RIP, Error, Vector, R15...RAX */
    pushq $0x23        /* SS */
    pushq 112(%rax)    /* RSP */
    pushq %r11         /* RFLAGS (stored by SYSCALL in R11) */
    pushq $0x1B        /* CS */
    pushq %rcx         /* RIP (stored by SYSCALL in RCX) */
    pushq $0           /* error_code */
    pushq $0x80        /* vector */

    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %rdx
    pushq %rcx
    pushq %rbx

    /* Restore and push RAX */
    movq x86_syscall_saved_user_rsp(%rip), %rax
    pushq %rax

    /* RSP now points to the start of the interrupt_frame (16-byte aligned).
     *
     * Acquire the Big Kernel Lock now that we are executing kernel code on
     * behalf of this userspace task. bkl_lock clobbers the caller-saved
     * registers that still hold the syscall arguments, so we reload every
     * argument from the saved register frame afterwards (offsets from the frame
     * start, which we keep in RBP). */
    call bkl_lock
    movq %rsp, %rbp    /* RBP = interrupt_frame pointer (also the 8th argument) */

    /* Syscall convention -> System V ABI argument registers. Frame layout (from
     * the pushes above): rax@0 rbx@8 rcx@16 rdx@24 rbp@32 rdi@40 rsi@48 r8@56
     * r9@64 r10@72 r11@80 ...  Syscall args are number=rax, arg0=rdi, arg1=rsi,
     * arg2=rdx, arg3=r10, arg4=r8, arg5=r9. */
    movq 0(%rbp),  %rdi    /* number */
    movq 40(%rbp), %rsi    /* arg0 (saved rdi) */
    movq 48(%rbp), %rdx    /* arg1 (saved rsi) */
    movq 24(%rbp), %rcx    /* arg2 (saved rdx) */
    movq 72(%rbp), %r8     /* arg3 (saved r10) */
    movq 56(%rbp), %r9     /* arg4 (saved r8) */

    /* System V stack arguments (pushed in reverse order). */
    pushq %rbp            /* 8th arg: frame pointer */
    pushq 64(%rbp)        /* 7th arg: arg5 (saved r9) */

    call syscall_dispatch_impl

.global x86_syscall_return
x86_syscall_return:
    /* Clean up the 2 stack arguments */
    addq $16, %rsp

    /* Store the return value into the saved RAX slot in the interrupt frame */
    movq %rax, (%rsp)

    /* Release the Big Kernel Lock before returning to ring 3 — userspace runs
     * lock-free so other cores can execute their own userspace in parallel. The
     * return value is parked in the frame slot above, so bkl_unlock clobbering
     * RAX is harmless (it is reloaded by the popq below). RSP points at the
     * 16-byte-aligned frame start here, satisfying the call ABI. */
    call bkl_unlock

    /* Restore GPRs from the interrupt_frame */
    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rbp
    popq %rdi
    popq %rsi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    /* Clean up vector and error code */
    addq $16, %rsp

    /* Restore RIP, CS, RFLAGS, RSP, SS (pushed manually) */
    popq %rcx   /* RIP */
    addq $8, %rsp /* Skip CS */
    popq %r11   /* RFLAGS */
    popq %rsp   /* RSP */
    /* Skip SS */

    sysretq

.global x86_fork_child_trampoline
x86_fork_child_trampoline:
    /* In the child, fork() returns 0 */
    xorq %rax, %rax
    jmp x86_syscall_return

.global x86_fork_kernel_trampoline
x86_fork_kernel_trampoline:
    xorq %rax, %rax
    movq %rbp, %rsp
    popq %rbp
    ret
