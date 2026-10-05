/* SPDX-License-Identifier: GPL-2.0-only */
// kernel/arch/aarch64/user_nofault.S — a user load that must not be serviced.
//
// See kernel/arch/x86_64/user_nofault.S: an EL1 abort on the load below is
// resumed at the fixup, which returns -EFAULT, instead of being handled as a
// page fault that may sleep.

.text
.global arch_user_load_u32_nofault
.global user_load_u32_nofault_insn
.global user_load_u32_nofault_fixup

// int arch_user_load_u32_nofault(const void *uaddr, u32 *out)
arch_user_load_u32_nofault:
user_load_u32_nofault_insn:
	ldr	w2, [x0]
	str	w2, [x1]
	mov	w0, #0
	ret
user_load_u32_nofault_fixup:
	mov	w0, #-14		// -EFAULT
	ret
