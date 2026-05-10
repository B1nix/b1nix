.section .text
.code64

.extern syscall_dispatch

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
    
    /* Restore RAX from scratch */
    movq x86_syscall_saved_user_rsp(%rip), %rax

    pushq %rcx  /* Return RIP */
    pushq %r11  /* Return RFLAGS */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Setup arguments for syscall_dispatch(number, arg0, arg1, arg2, arg3, arg4, arg5) */
    /* Registers at entry: RAX=num, RDI=a0, RSI=a1, RDX=a2, R10=a3, R8=a4, R9=a5 */
    
    /* We need to push arg5 (R9) onto the stack as the 7th argument for C */
    /* Fixed: Add dummy push to maintain 16-byte stack alignment for System V ABI */
    pushq $0
    pushq %r9
    
    /* Move others into correct positions for System V ABI */
    movq %r8, %r9   /* arg4 -> R9 (6th arg) */
    movq %r10, %r8  /* arg3 -> R8 (5th arg) */
    movq %rdx, %rcx /* arg2 -> RCX (4th arg) */
    movq %rsi, %rdx /* arg1 -> RDX (3rd arg) */
    movq %rdi, %rsi /* arg0 -> RSI (2nd arg) */
    movq %rax, %rdi /* number -> RDI (1st arg) */
    
    call syscall_dispatch
    
.global x86_syscall_return
x86_syscall_return:
    /* Clean up the 7th argument and padding from stack */
    addq $16, %rsp
    
    /* Restore registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    popq %r11
    popq %rcx

    /* Restore user RSP from current_task->saved_user_rsp */
    movq %rax, x86_syscall_saved_user_rsp(%rip) /* Save return value temporarily */
    movq current_task(%rip), %rax
    movq 112(%rax), %rsp
    movq x86_syscall_saved_user_rsp(%rip), %rax /* Restore return value */
    
    sysretq

.global x86_fork_child_trampoline
x86_fork_child_trampoline:
    /* In the child, fork() returns 0 */
    xorq %rax, %rax
    jmp x86_syscall_return
