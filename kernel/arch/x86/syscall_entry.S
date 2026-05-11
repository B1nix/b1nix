.section .text
.code64

.extern syscall_dispatch_impl

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

.extern current_task

/* SYSCALL entry point */
.global x86_syscall_entry
.type x86_syscall_entry, @function
x86_syscall_entry:
    /* Save RAX temporarily in our scratch area */
    movq %rax, x86_syscall_saved_user_rsp(%rip)
    
    /* Get current_task pointer */
    movq current_task(%rip), %rax
    
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

    /* RSP now points to the start of the interrupt_frame.
     * We pass this pointer as the 8th argument to syscall_dispatch.
     * Arguments: number(RDI), arg0(RSI), arg1(RDX), arg2(RCX), arg3(R8), arg4(R9), [arg5, frame]
     */
    
    /* Setup stack arguments (arg5 and frame) */
    movq %rsp, %rbp    /* Save frame pointer in RBP temporarily */
    
    /* Arguments for System V ABI:
     * RDI: number
     * RSI: arg0
     * RDX: arg1
     * RCX: arg2
     * R8:  arg3
     * R9:  arg4
     * [stack]: arg5
     * [stack+8]: frame
     */
     
    /* Push arguments in reverse order (System V ABI stack arguments) */
    pushq %rbp         /* 8th arg: frame pointer */
    
    /* Save arg5 (original R9) before we overwrite it */
    movq %r9, x86_syscall_saved_user_rsp(%rip)
    pushq x86_syscall_saved_user_rsp(%rip) /* 7th arg: arg5 */
    
    /* Move others into correct positions */
    movq %r8, %r9      /* arg4 -> R9 */
    movq %r10, %r8     /* arg3 -> R8 */
    movq %rdx, %rcx    /* arg2 -> RCX */
    movq %rsi, %rdx    /* arg1 -> RDX */
    movq %rdi, %rsi    /* arg0 -> RSI */
    movq %rax, %rdi    /* number -> RDI */
    
    call syscall_dispatch_impl
    
.global x86_syscall_return
x86_syscall_return:
    /* Clean up the 2 stack arguments */
    addq $16, %rsp
    
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
