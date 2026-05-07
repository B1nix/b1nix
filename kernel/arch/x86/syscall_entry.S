.section .text
.code64

.extern syscall_dispatch

/* SYSCALL entry point */
.global x86_syscall_entry
.type x86_syscall_entry, @function
x86_syscall_entry:
    /* Swap GS to kernel if needed (B1NIX doesn't use GS for TLS yet) */
    /* For now, just save registers */
    
    /* SYSCALL convention:
       %rdi = syscall number
       %rsi, %rdx, %r10, %r8, %r9 = args
       %rcx = return RIP (saved by CPU)
       %r11 = return RFLAGS (saved by CPU)
       Return value in %rax
    */
    
    /* B1NIX syscall_dispatch convention:
       syscall_dispatch(num, a0, a1, a2, a3)
       Wait, syscall_dispatch in syscall.c takes 5 args.
    */
    
    /* We need to save all caller-saved registers */
    pushq %rcx
    pushq %r11
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    
    /* Align stack and call C dispatcher */
    /* %rdi is already syscall num */
    /* %rsi is arg0 */
    /* %rdx is arg1 */
    /* %r10 -> %rcx (arg2) */
    /* %r8  is arg3 */
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
    
    sysretq
