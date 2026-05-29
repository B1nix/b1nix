.section .text
.code64

/* void x86_user_jump(u64 entry, u64 stack, u64 argc, u64 argv); */
.global x86_user_jump
.type x86_user_jump, @function
x86_user_jump:
    /* %rdi = entry, %rsi = stack, %rdx = argc, %rcx = argv */
    
    /* Set up segment registers for Ring 3. We deliberately do NOT reload %gs:
     * in 64-bit mode loading a GS selector resets the GS base, and the kernel
     * keeps its per-CPU base in GS (no SWAPGS) so the syscall handler can read
     * the per-CPU current task via %gs:0x10. b1nix userspace does not use %gs. */
    mov $0x1B, %ax /* User Data selector (0x18 | 3) */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs

    /* Prepare stack frame for iretq */
    /* [SS] [RSP] [RFLAGS] [CS] [RIP] */
    pushq $0x1B      /* SS */
    pushq %rsi       /* RSP */
    
    /* RFLAGS: enable interrupts (0x200) */
    pushfq
    popq %rax
    orq $0x200, %rax
    pushq %rax       /* RFLAGS */
    
    pushq $0x23      /* CS (User Code selector 0x20 | 3) */
    pushq %rdi       /* RIP */
    
    /* Pass arguments to user main */
    /* Following SysV ABI: %rdi = argc, %rsi = argv */
    movq %rdx, %rdi
    movq %rcx, %rsi
    
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rdx, %rdx
    xorq %rcx, %rcx
    xorq %rbp, %rbp
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    
    iretq
