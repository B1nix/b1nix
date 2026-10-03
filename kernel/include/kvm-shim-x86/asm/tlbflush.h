/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_TLBFLUSH_H
#define KVM_SHIM_ASM_TLBFLUSH_H
/* CR4 and CR3 as KVM touches them (M131). b1nix keeps no per-CPU CR4
 * shadow, so the "shadow" is the register itself; KVM sets CR4.VMXE through
 * cr4_set_bits before VMXON and clears it after VMXOFF. */
#include <linux/types.h>
#include <asm/special_insns.h>
#include <asm/processor-flags.h>

static inline unsigned long cr4_read_shadow(void) { return native_read_cr4(); }
static inline void cr4_set_bits(unsigned long mask)
{
	unsigned long flags;

	__asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
	native_write_cr4(native_read_cr4() | mask);
	if (flags & X86_EFLAGS_IF)
		__asm__ volatile("sti" ::: "memory");
}
static inline void cr4_clear_bits(unsigned long mask)
{
	unsigned long flags;

	__asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
	native_write_cr4(native_read_cr4() & ~mask);
	if (flags & X86_EFLAGS_IF)
		__asm__ volatile("sti" ::: "memory");
}
#define cr4_set_bits_irqsoff(m)   native_write_cr4(native_read_cr4() | (m))
#define cr4_clear_bits_irqsoff(m) native_write_cr4(native_read_cr4() & ~(m))
static inline unsigned long __get_current_cr3_fast(void) { return __native_read_cr3(); }
#define INVPCID_TYPE_INDIV_ADDR      0
#define INVPCID_TYPE_SINGLE_CTXT     1
#define INVPCID_TYPE_ALL_INCL_GLOBAL 2
#define INVPCID_TYPE_ALL_NON_GLOBAL  3
#endif
