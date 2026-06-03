.section .text
.code32

/* arch_context_switch(struct cpu_context *old, struct cpu_context *new,
 *                     volatile int *released_publish)
 * x86 32-bit calling convention: parameters passed on stack.
 */
.global arch_context_switch
.type arch_context_switch, @function
arch_context_switch:
	/* Fetch arguments from the stack */
	movl 4(%esp), %eax   /* old */
	movl 8(%esp), %ecx   /* new */
	movl 12(%esp), %edx  /* released_publish */

	/* Save callee-saved registers into old */
	movl %esp, 0(%eax)
	movl %ebp, 4(%eax)
	movl %ebx, 8(%eax)
	movl %esi, 12(%eax)
	movl %edi, 16(%eax)

	/* Switch to new stack */
	movl 0(%ecx), %esp

	/* Publish released_publish */
	testl %edx, %edx
	jz 1f
	movl $1, (%edx)
1:

	/* Restore callee-saved registers from new */
	movl 4(%ecx), %ebp
	movl 8(%ecx), %ebx
	movl 12(%ecx), %esi
	movl 16(%ecx), %edi

	ret
