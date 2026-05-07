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

/* SYSCALL entry point */
.global x86_syscall_entry
.type x86_syscall_entry, @function
x86_syscall_entry:
    /* SYSCALL convention:
       %rdi = syscall number
       %rsi, %rdx, %r10, %r8, %r9 = args
       %rcx = return RIP (saved by CPU)
       %r11 = return RFLAGS (saved by CPU)
       Return value in %rax
    */

    movq %rsp, x86_syscall_saved_user_rsp(%rip)
    leaq x86_syscall_stack_top(%rip), %rsp

    pushq %rcx
    pushq %r11
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    /*
     * B1NIX syscall_dispatch(num, a0, a1, a2, a3):
     * %rdi already holds num, %rsi/%rdx are a0/a1, %r10 becomes a2,
     * and %r8 is already the fifth SysV argument.
     */
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

    movq x86_syscall_saved_user_rsp(%rip), %rsp
    
    sysretq
