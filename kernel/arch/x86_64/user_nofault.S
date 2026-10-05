/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel/arch/x86_64/user_nofault.S — a user load that must not be serviced.
 *
 * Code holding a spinlock may not take a page fault that sleeps, yet sometimes
 * has to read a user word under that lock (the futex re-check). This load is
 * the one instruction the page-fault handler recognises: a fault on it is not
 * serviced but resumed at the fixup, which returns -EFAULT, and the caller
 * drops its lock, faults the page in the ordinary way and tries again. The
 * same idea as FreeBSD's pcb_onfault and Linux's exception table, for the one
 * access that needs it. */

.text
.global arch_user_load_u32_nofault
.global user_load_u32_nofault_insn
.global user_load_u32_nofault_fixup

/* int arch_user_load_u32_nofault(const void *uaddr, u32 *out) */
arch_user_load_u32_nofault:
user_load_u32_nofault_insn:
	movl (%rdi), %eax
	movl %eax, (%rsi)
	xorl %eax, %eax
	ret
user_load_u32_nofault_fixup:
	movl $-14, %eax		/* -EFAULT */
	ret
