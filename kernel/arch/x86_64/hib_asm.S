/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hibernation's two pieces of assembly (M135, kernel/power/hibernate.c).
 *
 * x86_hib_setjmp saves where the snapshot was taken: the callee-saved
 * registers, the stack pointer and return address as a return would leave
 * them, and CR3. It returns 0 there, and 1 when the restored image comes back
 * through x86_hib_restore.
 *
 * x86_hib_restore is the last thing the boot kernel does. It runs on a stack
 * and page tables that live in frames the image does not use, copies every
 * image page from where the boot kernel staged it to where it belongs, loads
 * the image's CR3 and jumps into the saved context. The kernel text it runs
 * from is overwritten on the way with the same bytes -- the image and the boot
 * kernel are the same binary -- and nothing else of the boot kernel is read.
 */

	.text
	.code64

/* struct hib_jmp: rbx rbp r12 r13 r14 r15 rsp rip cr3 */
#define J_RBX 0
#define J_RBP 8
#define J_R12 16
#define J_R13 24
#define J_R14 32
#define J_R15 40
#define J_RSP 48
#define J_RIP 56
#define J_CR3 64

	.globl x86_hib_setjmp
	.type x86_hib_setjmp, @function
/* int x86_hib_setjmp(struct hib_jmp *j) */
x86_hib_setjmp:
	movq %rbx, J_RBX(%rdi)
	movq %rbp, J_RBP(%rdi)
	movq %r12, J_R12(%rdi)
	movq %r13, J_R13(%rdi)
	movq %r14, J_R14(%rdi)
	movq %r15, J_R15(%rdi)
	leaq 8(%rsp), %rax
	movq %rax, J_RSP(%rdi)
	movq (%rsp), %rax
	movq %rax, J_RIP(%rdi)
	movq %cr3, %rax
	movq %rax, J_CR3(%rdi)
	xorl %eax, %eax
	ret
	.size x86_hib_setjmp, . - x86_hib_setjmp

	.globl x86_hib_restore
	.type x86_hib_restore, @function
/* void x86_hib_restore(u64 temp_cr3, u64 stack_top, u64 chain, struct hib_jmp *j)
 *
 * `chain` is the direct-map address of the first page of copy orders: each
 * page is { u64 next; u64 count; struct { u64 dst, src; } pair[255]; }, every
 * address a direct-map one. Never returns. */
x86_hib_restore:
	cli
	movq %rsi, %rsp
	movq %rcx, %r15
	/* No global translations may survive the switch: the image's own are
	 * rebuilt from its page tables, the boot kernel's are about to be wrong. */
	movq %cr4, %rax
	movq %rax, %r14
	andq $~0x80, %rax
	movq %rax, %cr4
	movq %rdi, %cr3
	movq %rdx, %r13
1:	testq %r13, %r13
	jz 3f
	movq 8(%r13), %r8
	leaq 16(%r13), %r9
2:	testq %r8, %r8
	jz 4f
	movq 0(%r9), %rdi
	movq 8(%r9), %rsi
	movl $512, %ecx
	cld
	rep movsq
	addq $16, %r9
	decq %r8
	jmp 2b
4:	movq 0(%r13), %r13
	jmp 1b
3:	/* The image is in place: its page tables, then its registers. */
	movq J_CR3(%r15), %rax
	movq %rax, %cr3
	movq %r14, %cr4
	movq J_RBX(%r15), %rbx
	movq J_RBP(%r15), %rbp
	movq J_R12(%r15), %r12
	movq J_R13(%r15), %r13
	movq J_R14(%r15), %r14
	movq J_RSP(%r15), %rsp
	movq J_RIP(%r15), %r11
	movq J_R15(%r15), %r15
	movl $1, %eax
	jmp *%r11
	.size x86_hib_restore, . - x86_hib_restore

	.section .note.GNU-stack,"",@progbits
