.section .text
.code64

.extern syscall_dispatch_impl

/* The syscall entry stack, and the guard that catches it running off the
 * bottom.
 *
 * Same defect the boot stack had, and worse placed: this one sat immediately
 * above stack_top, so an overflow here ran straight down into the boot CPU's
 * stack -- silently, because that is ordinary writable .bss too. The syscall
 * path is not shallow: it carries a 28,584-byte frame, so 64 KiB left little
 * margin for anything nested behind it.
 *
 * 256 KiB with a 32 KiB unmapped guard below, armed by
 * paging_install_guard_page() once the kernel window exists. The guard is
 * several pages for the same reason the boot stack's is: a guard a single
 * frame can step over is not a guard.
 *
 * Page-aligned and global because the guard has to be whole pages and the
 * installer needs both ends. */
.section .bss
.align 4096
.global x86_syscall_stack_guard
x86_syscall_stack_guard:
    .skip 32768
.global x86_syscall_stack_guard_end
x86_syscall_stack_guard_end:

.align 4096
.global x86_syscall_stack_bottom
x86_syscall_stack_bottom:
    .skip 262144
.global x86_syscall_stack_top
x86_syscall_stack_top:

.section .text
.code64

/* SYSCALL entry point */
.global x86_syscall_entry
.type x86_syscall_entry, @function
x86_syscall_entry:
    /* Save RAX temporarily in per-CPU scratch. This runs before the BKL is
     * acquired, so a global scratch slot would race between simultaneous
     * syscalls on different CPUs. */
    movq %rax, %gs:0x60

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
    pushq $0x1B        /* SS */
    pushq 112(%rax)    /* RSP */
    pushq %r11         /* RFLAGS (stored by SYSCALL in R11) */
    pushq $0x23        /* CS */
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
    movq %gs:0x60, %rax
    pushq %rax

    /* RSP now points to the start of the interrupt_frame (16-byte aligned).
     * Kernel entry runs BKL-free (M28 #7); per-subsystem locks provide all
     * SMP serialisation. */
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

    /*
     * Interrupts on for the body of the call.
     *
     * SYSCALL clears IF through IA32_FMASK, and nothing turned it back on — so
     * every system call ran to completion with interrupts disabled. The kernel
     * tolerated that because its own blocking paths enable them by hand, but a
     * driver does not: imported code asks "may I sleep here?", is told no
     * because interrupts are off, and falls back to polling a timeout that can
     * never expire, since the timer that advances it is masked. i915 probing a
     * connector over GMBUS hung exactly there.
     *
     * Enabled here, after every register is saved and the kernel stack is
     * established, and cleared again before SYSRETQ below.
     */
    sti

    call syscall_dispatch_impl

.global x86_syscall_return
x86_syscall_return:
    /* Clean up the 2 stack arguments */
    addq $16, %rsp

    /* Store the return value into the saved RAX slot in the interrupt frame */
    movq %rax, (%rsp)

    /* If vector != 0x80, return via iretq to preserve rcx and r11 (e.g. from sys_sigreturn) */
    cmpq $0x80, 120(%rsp)
    jne .Liret_return

    /* BKL fully retired (M28 #7): no lock to release on the way back to ring 3. */

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

    /* Between restoring the user RSP and SYSRETQ the CPU is still at CPL 0.
     * An interrupt taken there does NOT reload RSP0 from the TSS, so it would
     * be delivered onto the *user* stack and the handler would run in ring 0
     * on user memory — the moment it reaches a context switch into a kernel
     * task, CR3 flips to the kernel PML4, the user stack vanishes and the
     * return sequence double-faults. Mask interrupts across the window;
     * SYSRETQ restores RFLAGS from R11, which still carries the user's IF. */
    cli
    popq %rsp   /* RSP */
    /* Skip SS */

    sysretq

.Liret_return:
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
    addq $16, %rsp   /* Skip vector and error code */
    iretq

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
