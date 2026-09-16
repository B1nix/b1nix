/*
 * The vDSO's two machine-level helpers on x86_64: the system call it falls back
 * to, and the counter read. Both are leaf functions with standard CFI so an
 * unwinder can walk through them.
 */
	.text

/* long __b1nix_vdso_syscall2(long nr, long a0, long a1)
 * The Linux x86_64 convention: number in %rax, arguments in %rdi/%rsi. The
 * instruction clobbers %rcx and %r11, both call-clobbered. */
	.globl __b1nix_vdso_syscall2
	.hidden __b1nix_vdso_syscall2
	.type __b1nix_vdso_syscall2, @function
__b1nix_vdso_syscall2:
	.cfi_startproc
	movq %rdi, %rax
	movq %rsi, %rdi
	movq %rdx, %rsi
	syscall
	ret
	.cfi_endproc
	.size __b1nix_vdso_syscall2, . - __b1nix_vdso_syscall2

/* u64 __b1nix_vdso_counter(void)
 * The time-stamp counter, read the way the kernel's arch_rdtsc_ordered reads
 * it: LFENCE first, so a speculatively executed RDTSC cannot return a value
 * older than one already observed on this path. */
	.globl __b1nix_vdso_counter
	.hidden __b1nix_vdso_counter
	.type __b1nix_vdso_counter, @function
__b1nix_vdso_counter:
	.cfi_startproc
	lfence
	rdtsc
	shlq $32, %rdx
	orq %rdx, %rax
	ret
	.cfi_endproc
	.size __b1nix_vdso_counter, . - __b1nix_vdso_counter

	.section .note.GNU-stack, "", @progbits
