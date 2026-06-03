.section .text
.code32

.extern syscall_dispatch_impl

.section .bss
.align 16
.global x86_syscall_stack_bottom
x86_syscall_stack_bottom:
    .skip 65536
.global x86_syscall_stack_top
x86_syscall_stack_top:

.section .text
.code32

/* x86_syscall_entry: System call handler for vector 0x80 (int $0x80) */
.global x86_syscall_entry
.type x86_syscall_entry, @function
x86_syscall_entry:
	/* CPU has pushed: SS, ESP, EFLAGS, CS, EIP */
	pushl $0      /* error_code */
	pushl $0x80   /* vector */

	/* Save other registers to complete the interrupt_frame */
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax

	/* Reload segment registers for kernel mode */
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %gs
	movw $0x38, %ax
	movw %ax, %fs

	/* Now ESP points to the start of struct interrupt_frame.
	 * We need to call syscall_dispatch_impl.
	 * The SysV ABI on 32-bit x86 passes arguments on the stack.
	 * syscall_dispatch_impl expects 64-bit values for the first 7 arguments.
	 * We also want the stack to be 16-byte aligned before the call.
	 * S (interrupt_frame address) is 8-byte aligned.
	 * Pushing 15 arguments (60 bytes) + 12 bytes of padding = 72 bytes.
	 * S - 72 is 16-byte aligned.
	 */
	subl $12, %esp       /* stack alignment padding */

	/* 8. frame pointer (interrupt_frame is at S, which is now ESP + 12) */
	leal 12(%esp), %eax
	pushl %eax

	/* 7. arg5 (ebp) */
	pushl $0
	pushl %ebp

	/* 6. arg4 (edi) */
	pushl $0
	pushl %edi

	/* 5. arg3 (esi) */
	pushl $0
	pushl %esi

	/* 4. arg2 (edx) */
	pushl $0
	pushl %edx

	/* 3. arg1 (ecx) */
	pushl $0
	pushl %ecx

	/* 2. arg0 (ebx) */
	pushl $0
	pushl %ebx

	/* 1. number (saved eax is at S + 0, which is ESP + 64 before pushl $0) */
	pushl $0
	pushl 68(%esp)

	call syscall_dispatch_impl
	addl $72, %esp       /* Clean up parameters + padding */

.global x86_syscall_return
x86_syscall_return:
	/* EAX holds the return value. Store it into the saved EAX slot. */
	movl %eax, 0(%esp)

	/* If returning to user mode (CS RPL == 3), restore the user segment
	 * selectors — BEFORE the GP-register pops. The segment loads go through
	 * %ax; doing them after `popl %eax` overwrote the low 16 bits of the
	 * just-restored return value with 0x33, silently corrupting EVERY ring-3
	 * syscall result (fork()'s 0 became 0x33 so children took the parent
	 * branch, pids/byte-counts were mangled, …). EAX is still scratch here, so
	 * clobbering %ax is harmless — the trailing `popl %eax` restores the clean
	 * value. CS is at offset 40 before any pops:
	 *   eax0 ebx4 ecx8 edx12 ebp16 edi20 esi24 vec28 err32 eip36 cs40 */
	testl $3, 40(%esp)
	jz 1f
	movw $0x23, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw $0x33, %ax
	movw %ax, %gs
1:
	/* Restore registers from the interrupt_frame */
	popl %eax
	popl %ebx
	popl %ecx
	popl %edx
	popl %ebp
	popl %edi
	popl %esi

	/* Clean up vector and error code */
	addl $8, %esp

	/* Return to user mode */
	iret

.global x86_fork_child_trampoline
x86_fork_child_trampoline:
	/* In the child, fork() returns 0 */
	xorl %eax, %eax
	jmp x86_syscall_return

.global x86_fork_kernel_trampoline
x86_fork_kernel_trampoline:
	xorl %eax, %eax
	movl %ebp, %esp
	popl %ebp
	ret
