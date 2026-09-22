/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The two halves of an S3 sleep that cannot be written in C (M129).
 *
 * A sleep leaves the processor and comes back to it somewhere else: the write
 * to PM1_CNT does not return, and what resumes is a real-mode trampoline with
 * no stack, no long mode and no registers. So the register state has to be
 * PUSHED before the write and POPPED after the wake, with the stack pointer
 * handed to the trampoline as the one thing that ties the two together — a
 * setjmp/longjmp in the two directions the hardware allows.
 *
 * Nothing here decides anything: which registers to write and what to write in
 * them comes from kernel/arch/x86_64/s3.c, which read it out of the firmware's
 * own tables.
 */

    .text

/* int x86_s3_sleep_and_wake(u32 pm1a_port, u32 pm1b_port, u32 val_a, u32 val_b,
 *                           u64 *stack_slot)
 *
 * Returns 1 when the machine really slept and came back through the trampoline,
 * and 0 when the write to PM1_CNT returned — which a machine whose firmware
 * does not implement the state does, and which must not be reported as a sleep.
 */
    .global x86_s3_sleep_and_wake
x86_s3_sleep_and_wake:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushfq
    /* The trampoline resumes on this stack, at the frame these pushes just
     * made: everything the caller expects to survive the sleep is in it. */
    movq %rsp, (%r8)

    /* Caches are about to be lost, so whatever is only in them has to reach
     * memory first -- including the words the trampoline is going to read. */
    wbinvd

    /* PM1a_CNT, then PM1b_CNT where the platform has a second half. The write
     * that enters the state is the one to PM1a; on a split block both must be
     * written, and the specification has PM1b written second. */
    movl %edx, %eax             /* val_a */
    movl %edi, %edx             /* pm1a_port */
    outw %ax, %dx
    movl %esi, %edx             /* pm1b_port */
    testl %edx, %edx
    jz 1f
    movl %ecx, %eax             /* val_b */
    outw %ax, %dx
1:
    /* Give the platform a moment to act. A machine that is going to sleep does
     * so inside this loop and never reaches the end of it; one that returns
     * from the write has refused the state, and that is what 0 reports.
     *
     * A million pauses, not sixteen million: under a hypervisor that intercepts
     * PAUSE (every KVM guest, for spin-loop exiting) each one is an exit costing
     * about a microsecond, so the longer loop spent a minute deciding that a
     * refused sleep had been refused — long enough for a test harness to give up
     * on the machine and call it hung. */
    movl $0x00100000, %ecx
2:
    pause
    decl %ecx
    jnz 2b
    xorl %eax, %eax
    jmp 4f

/* Entered from the wake-up trampoline with RSP already restored to the frame
 * above, interrupts off, and nothing else of the kernel's processor state in
 * place. The C call puts the descriptor tables, the task register and the
 * segment MSRs back before anything can fault or interrupt. */
    .global x86_s3_resume_point
x86_s3_resume_point:
    /* The trampoline got us here on ITS code selector (0x18 in its own small
     * GDT) and with the kernel's GDT loaded, where 0x18 is something else
     * entirely. The processor is still running on the cached descriptor, so it
     * works right up to the first operation that reloads CS — an interrupt
     * return, a syscall, a far call — and that one faults with the selector in
     * the error code. So CS is reloaded from the kernel GDT here, with a far
     * return, exactly as an AP does when it leaves its own trampoline; the data
     * segments follow. Not %gs: reloading a GS selector resets GS_BASE, which is
     * where this CPU's per-CPU pointer lives, and the C side puts it back from
     * the MSR a moment later. */
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    xorq %rbp, %rbp
    call x86_s3_restore_cpu
    movl $1, %eax
4:
    popfq
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
