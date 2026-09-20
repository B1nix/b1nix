/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The vDSO's two machine-level helpers on aarch64: the system call it falls
 * back to, and the counter read. Both are leaf functions with standard CFI so
 * an unwinder can walk through them.
 */
	.text

/* long __b1nix_vdso_syscall2(long nr, long a0, long a1)
 * The Linux arm64 convention: number in x8, arguments in x0/x1. The kernel
 * preserves every register except x0, the link register included. */
	.globl __b1nix_vdso_syscall2
	.hidden __b1nix_vdso_syscall2
	.type __b1nix_vdso_syscall2, %function
__b1nix_vdso_syscall2:
	.cfi_startproc
	mov x8, x0
	mov x0, x1
	mov x1, x2
	svc #0
	ret
	.cfi_endproc
	.size __b1nix_vdso_syscall2, . - __b1nix_vdso_syscall2

/* u64 __b1nix_vdso_counter(void)
 * The generic timer's virtual counter. Readable at EL0 because the kernel sets
 * CNTKCTL_EL1.EL0VCTEN on every CPU before that CPU runs a process. */
	.globl __b1nix_vdso_counter
	.hidden __b1nix_vdso_counter
	.type __b1nix_vdso_counter, %function
__b1nix_vdso_counter:
	.cfi_startproc
	mrs x0, cntvct_el0
	ret
	.cfi_endproc
	.size __b1nix_vdso_counter, . - __b1nix_vdso_counter

	.section .note.GNU-stack, "", %progbits
