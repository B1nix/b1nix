.section .text
.code64

/* arch_context_switch(struct cpu_context *old, struct cpu_context *new,
 *                     volatile int *released_publish)
 *   rdi = &old->context
 *   rsi = &new->context
 *   rdx = pointer to int that this routine stores 1 into AFTER the RSP swap
 *         (used by scheduler_waitpid to know the old task's kernel_stack is
 *         no longer being used as RSP on this CPU). NULL ⇒ skipped — for
 *         callers where the "old" side is a stack-local cpu_context (e.g. the
 *         AP idle context in lapic.c::ap_main) that is never reaped.
 *
 * The publish MUST happen after the RSP swap, otherwise a polling reaper
 * could observe `released == 1` while this CPU is still pushing to the old
 * stack. See struct task::stack_released in kernel/include/b1nix/sched.h
 * for the full reasoning.
 */
.global arch_context_switch
.type arch_context_switch, @function
arch_context_switch:
	movq %rsp, 0(%rdi)
	movq %rbp, 8(%rdi)
	movq %rbx, 16(%rdi)
	movq %r12, 24(%rdi)
	movq %r13, 32(%rdi)
	movq %r14, 40(%rdi)
	movq %r15, 48(%rdi)

	movq 0(%rsi), %rsp

/* M29: MSR_FS_BASE write is handled in C by scheduler_yield (it has the
 * task pointer; this asm only knows about cpu_context). See arch_set_fs_base
 * in kernel/arch/x86_64/arch.c. */

	/* Publish "old kernel stack is no longer in use as RSP" — only after
	 * the swap above, so no reaper can observe released=1 while we're
	 * still on the old stack. rdx is allowed to be NULL. */
	testq %rdx, %rdx
	jz 1f
	movl $1, (%rdx)
1:

	movq 8(%rsi), %rbp
	movq 16(%rsi), %rbx
	movq 24(%rsi), %r12
	movq 32(%rsi), %r13
	movq 40(%rsi), %r14
	movq 48(%rsi), %r15
	ret
