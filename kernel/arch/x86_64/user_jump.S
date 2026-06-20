.section .text
.code64

/* void x86_user_jump(u64 entry, u64 stack, u64 argc, u64 argv); */
.global x86_user_jump
.type x86_user_jump, @function
x86_user_jump:
    /* %rdi = entry, %rsi = stack, %rdx = argc, %rcx = argv */

    /* First entry into ring 3 for this task. The BKL is fully retired
     * (M28 #7) — there is no kernel lock to drop before dropping to ring 3. */

    /* Set up segment registers for Ring 3. The %fs selector must point at the
     * user-data descriptor (arch_set_fs_base relies on this), but in 64-bit mode
     * loading a segment selector RESETS that segment's base — which would zero
     * the userspace TLS base the loader/scheduler just programmed into the
     * IA32_FS_BASE MSR. A thread that reads %fs:N (a thread_local) before its
     * first context switch (which would otherwise restore the base) then faults
     * near null. So: snapshot the FS base, reload the selectors, then restore
     * the base via wrmsr. (We still do NOT reload %gs: the kernel keeps its
     * per-CPU base there with no SWAPGS.) */
    /* rdmsr/wrmsr clobber eax/edx/ecx — which alias argc (%rdx) and argv (%rcx),
     * still needed below. Stash both in scratch (r10/r11, zeroed before iretq). */
    movq %rcx, %r10        /* save argv */
    movq %rdx, %r11        /* save argc */
    movl $0xC0000100, %ecx /* IA32_FS_BASE */
    rdmsr                  /* edx:eax = current FS base */
    movl %eax, %r8d
    movl %edx, %r9d
    mov $0x1B, %ax /* User Data selector (0x18 | 3) */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs           /* sets selector, zeroes FS base */
    movl %r8d, %eax
    movl %r9d, %edx
    movl $0xC0000100, %ecx
    wrmsr                  /* restore FS base the loader/scheduler set */
    movq %r10, %rcx        /* restore argv */
    movq %r11, %rdx        /* restore argc */

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
