/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_APIC_H
#define KVM_SHIM_ASM_APIC_H
/* The host local APIC as KVM uses it (M131): the register layout comes from
 * Linux's apicdef.h, the IPIs are sent by b1nix's LAPIC driver. */
#include <linux/types.h>
#include <linux/cpumask.h>
#include <asm/apicdef.h>
#include <asm/irq_vectors.h>
#include <linux/bitops.h>

extern int x2apic_mode;
/* The APIC id of a CPU number, BAD_APICID when there is no such CPU. */
u32 default_cpu_present_to_apicid(int cpu);
void __apic_send_IPI(int cpu, int vector);
void __apic_send_IPI_mask(const struct cpumask *mask, int vector);
void __apic_send_IPI_self(int vector);
#define apic_send_IPI_allbutself(v) do { (void)(v); } while (0)

#define APIC_VECTOR_TO_BIT_NUMBER(v) ((unsigned int)(v) % 32)
#define APIC_VECTOR_TO_REG_OFFSET(v) ((unsigned int)(v) / 32 * 0x10)
#define MAX_APIC_VECTOR      256
#define APIC_VECTORS_PER_REG 32

static inline int apic_find_highest_vector(void *bitmap)
{
	int vec;
	u32 *reg;

	for (vec = MAX_APIC_VECTOR - APIC_VECTORS_PER_REG; vec >= 0; vec -= APIC_VECTORS_PER_REG) {
		reg = (u32 *)((char *)bitmap + APIC_VECTOR_TO_REG_OFFSET(vec));
		if (*reg)
			return __fls(*reg) + vec;
	}
	return -1;
}
static inline u32 apic_get_reg(void *regs, int reg) { return *((u32 *)((char *)regs + reg)); }
static inline void apic_set_reg(void *regs, int reg, u32 val) { *((u32 *)((char *)regs + reg)) = val; }
static inline u64 apic_get_reg64(void *regs, int reg) { return *((u64 *)((char *)regs + reg)); }
static inline void apic_set_reg64(void *regs, int reg, u64 val) { *((u64 *)((char *)regs + reg)) = val; }
static inline void apic_clear_vector(int vec, void *bitmap)
{
	clear_bit(APIC_VECTOR_TO_BIT_NUMBER(vec), (unsigned long *)((char *)bitmap + APIC_VECTOR_TO_REG_OFFSET(vec)));
}
static inline void apic_set_vector(int vec, void *bitmap)
{
	set_bit(APIC_VECTOR_TO_BIT_NUMBER(vec), (unsigned long *)((char *)bitmap + APIC_VECTOR_TO_REG_OFFSET(vec)));
}
static inline int apic_test_vector(int vec, void *bitmap)
{
	return test_bit(APIC_VECTOR_TO_BIT_NUMBER(vec), (unsigned long *)((char *)bitmap + APIC_VECTOR_TO_REG_OFFSET(vec)));
}
/* The handler for POSTED_INTR_WAKEUP_VECTOR (b1nix's IDT calls it). */
void kvm_set_posted_intr_wakeup_handler(void (*handler)(void));

#endif
