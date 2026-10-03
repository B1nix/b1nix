/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_BARRIER_H
#define KVM_SHIM_ARM64_ASM_BARRIER_H
/* arm64's barrier instructions, as upstream spells them (asm/barrier.h); the
 * smp_* orderings come from linuxkpi's generic header behind this one. */
#define __nops(n)	".rept	" #n "\nnop\n.endr\n"
#define nops(n)		asm volatile(__nops(n))
#define sev()		asm volatile("sev" : : : "memory")
#define wfe()		asm volatile("wfe" : : : "memory")
#define wfi()		asm volatile("wfi" : : : "memory")
#define isb()		asm volatile("isb" : : : "memory")
#define dmb(opt)	asm volatile("dmb " #opt : : : "memory")
#define dsb(opt)	asm volatile("dsb " #opt : : : "memory")
#define psb_csync()	asm volatile("hint #17" : : : "memory")
#define tsb_csync()	asm volatile("hint #18" : : : "memory")
#define csdb()		asm volatile("hint #20" : : : "memory")
#define spec_bar()	asm volatile("dsb nsh\nisb\n" : : : "memory")
#define pmr_sync()	do { } while (0)
/* Order a counter read before later memory accesses: a fake address
 * dependency on the value read. */
#define arch_counter_enforce_ordering(val) do {				\
	u64 tmp, _val = (val);						\
									\
	asm volatile(							\
	"	eor	%0, %1, %1\n"					\
	"	add	%0, sp, %0\n"					\
	"	ldr	xzr, [%0]"					\
	: "=r" (tmp) : "r" (_val));					\
} while (0)
#include_next <asm/barrier.h>
#endif
