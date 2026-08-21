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

/* void x86_clone_thread_jump(u64 entry, u64 stack, u64 start_func);
 * M92 musl pthread: drop to ring 3 at the parent's instruction pointer
 * (after the syscall in musl __clone), with rax=0 (clone return value)
 * and r9=start_func (start_routine pointer). musl's child code then pops
 * the arg from the user stack and calls *r9. */
.global x86_clone_thread_jump
.type x86_clone_thread_jump, @function
x86_clone_thread_jump:
    /* %rdi = entry (parent's RIP), %rsi = stack (user stack), %rdx = start_func */
    /* Save start_func before FS reload clobbers r11 */
    movq %rdx, %r11        /* r11 = start_func */

    /* Reload FS base (same as x86_user_jump) — loading a segment selector
     * zeroes the FS base, so snapshot+restore around the selector reload. */
    movq %rcx, %r10        /* stash rcx (clobbered by rdmsr/wrmsr) */
    movl $0xC0000100, %ecx
    rdmsr
    movl %eax, %r8d
    movl %edx, %r9d
    mov $0x1B, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    movl %r8d, %eax
    movl %r9d, %edx
    movl $0xC0000100, %ecx
    wrmsr
    movq %r10, %rcx

    /* Build iretq frame: SS, RSP, RFLAGS, CS, RIP */
    pushq $0x1B        /* SS (user data) */
    pushq %rsi         /* RSP (user stack) */
    pushfq
    popq %rax
    orq $0x200, %rax   /* enable interrupts */
    pushq %rax         /* RFLAGS */
    pushq $0x23        /* CS (user code) */
    pushq %rdi         /* RIP (parent's instruction pointer) */

    /* Set registers for musl child thread:
     * rax = 0  (clone() return value — child)
     * r9  = start_func  (musl's __clone stores fn in r9 for the child)
     * rsp = user stack  (set via iretq above)
     * rip = parent's RIP  (set via iretq above)
     * All other GPRs zeroed. rdi will be overwritten by musl's pop %rdi. */
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rbp, %rbp
    xorq %r8, %r8
    xorq %r10, %r10
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    xorq %rdi, %rdi
    xorq %rsi, %rsi
    movq %r11, %r9     /* r9 = start_func (start_routine) */
    xorq %r11, %r11

    iretq

/* void x86_clone_thread_jump_regs(u64 entry, u64 stack,
 *                                 const struct clone_user_regs *regs);
 *
 * Drop to ring 3 at `entry` (the instruction after the caller's syscall) on
 * `stack`, with the caller's whole register file restored and rax = 0.
 *
 * This is what Linux's clone(2) does, and libcs depend on it: glibc's
 * __clone3 leaves the thread function in %r9 and its argument in %r8 across
 * the syscall and uses them in the child, while its __clone pops both from the
 * new stack. Zeroing the registers instead called the entry point with a NULL
 * argument, or through a NULL pointer — every glibc program that creates a
 * thread died at rip=0.
 *
 * struct clone_user_regs is, in order:
 *   0 rbx  8 rcx 16 rdx 24 rsi 32 rdi 40 rbp
 *  48 r8  56 r9  64 r10 72 r11 80 r12 88 r13 96 r14 104 r15
 */
.global x86_clone_thread_jump_regs
.type x86_clone_thread_jump_regs, @function
x86_clone_thread_jump_regs:
    /* %rdi = entry, %rsi = stack, %rdx = regs */
    movq %rdx, %r15        /* the register block survives the FS dance here */

    /* Reload FS base: loading a segment selector zeroes the FS base, so
     * snapshot it and put it back (same dance as x86_user_jump). */
    movl $0xC0000100, %ecx
    rdmsr
    movl %eax, %r8d
    movl %edx, %r9d
    mov $0x1B, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    movl %r8d, %eax
    movl %r9d, %edx
    movl $0xC0000100, %ecx
    wrmsr

    /* iretq frame: SS, RSP, RFLAGS, CS, RIP */
    pushq $0x1B            /* SS (user data) */
    pushq %rsi             /* RSP (the new thread's stack) */
    pushfq
    popq %rax
    orq $0x200, %rax       /* interrupts on in ring 3 */
    pushq %rax             /* RFLAGS */
    pushq $0x23            /* CS (user code) */
    pushq %rdi             /* RIP */

    /* The caller's registers, r15 (which holds the block) last. */
    movq   0(%r15), %rbx
    movq   8(%r15), %rcx
    movq  16(%r15), %rdx
    movq  24(%r15), %rsi
    movq  32(%r15), %rdi
    movq  40(%r15), %rbp
    movq  48(%r15), %r8
    movq  56(%r15), %r9
    movq  64(%r15), %r10
    movq  72(%r15), %r11
    movq  80(%r15), %r12
    movq  88(%r15), %r13
    movq  96(%r15), %r14
    movq 104(%r15), %r15
    xorq %rax, %rax        /* clone(2) returns 0 in the child */
    iretq
