/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_SPECIAL_INSNS_H
#define KVM_SHIM_ASM_SPECIAL_INSNS_H
#ifndef __ASSEMBLY__
#include <linux/types.h>

static inline unsigned long native_read_cr0(void)
{
	unsigned long v;
	__asm__ volatile("mov %%cr0, %0" : "=r"(v));
	return v;
}
static inline unsigned long native_read_cr2(void)
{
	unsigned long v;
	__asm__ volatile("mov %%cr2, %0" : "=r"(v));
	return v;
}
static inline void native_write_cr2(unsigned long v)
{
	__asm__ volatile("mov %0, %%cr2" : : "r"(v) : "memory");
}
static inline unsigned long __native_read_cr3(void)
{
	unsigned long v;
	__asm__ volatile("mov %%cr3, %0" : "=r"(v));
	return v;
}
static inline unsigned long native_read_cr4(void)
{
	unsigned long v;
	__asm__ volatile("mov %%cr4, %0" : "=r"(v));
	return v;
}
static inline void native_write_cr0(unsigned long v)
{
	__asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}
static inline void native_write_cr4(unsigned long v)
{
	__asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

#define read_cr0()     native_read_cr0()
#define write_cr0(v)   native_write_cr0(v)
#define read_cr2()     native_read_cr2()
#define write_cr2(v)   native_write_cr2(v)
#define __read_cr3()   __native_read_cr3()
#define read_cr3_pa()  (__native_read_cr3() & ~0xfffUL)
#define __read_cr4()   native_read_cr4()

static inline void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }
static inline void native_wbinvd(void) { wbinvd(); }
static inline void clflush(volatile void *p)
{
	__asm__ volatile("clflush %0" : "+m"(*(volatile char *)p));
}

static inline u32 rdpkru(void)
{
	u32 ecx = 0, edx, pkru;
	__asm__ volatile(".byte 0x0f,0x01,0xee" : "=a"(pkru), "=d"(edx) : "c"(ecx));
	return pkru;
}
static inline void wrpkru(u32 pkru)
{
	__asm__ volatile(".byte 0x0f,0x01,0xef" : : "a"(pkru), "c"(0), "d"(0));
}

/* Load a GS selector without losing the kernel's GS base: the load writes the
 * base of whichever GS is active, so it is done with the user's swapped in,
 * interrupts off, as Linux's asm_load_gs_index does. */
static inline void native_load_gs_index(unsigned int sel)
{
	unsigned long flags;

	__asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
	__asm__ volatile("swapgs; movl %0, %%gs; swapgs" : : "r"(sel) : "memory");
	if (flags & (1UL << 9))
		__asm__ volatile("sti" ::: "memory");
}
#define load_gs_index(sel) native_load_gs_index(sel)
#endif
#endif
