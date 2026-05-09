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

    /* Setup arguments for syscall_dispatch */
    movq %r10, %rcx
    
    call syscall_dispatch
    
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
