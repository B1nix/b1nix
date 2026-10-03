/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_DESC_H
#define KVM_SHIM_ASM_DESC_H
/* Descriptor tables as KVM reads them for a VMCS's host-state area: this
 * CPU's GDT, IDT and TSS. b1nix has one GDT (with a TSS descriptor per CPU)
 * and one IDT. */
#include <linux/types.h>
#include <asm/desc_defs.h>
#include <asm/segment.h>

static inline void native_store_gdt(struct desc_ptr *dtr)
{
	__asm__ volatile("sgdt %0" : "=m"(*dtr));
}
static inline void store_idt(struct desc_ptr *dtr)
{
	__asm__ volatile("sidt %0" : "=m"(*dtr));
}
static inline void native_load_gdt(const struct desc_ptr *dtr)
{
	__asm__ volatile("lgdt %0" : : "m"(*dtr));
}
static inline void native_load_idt(const struct desc_ptr *dtr)
{
	__asm__ volatile("lidt %0" : : "m"(*dtr));
}
#define load_idt(dtr) native_load_idt(dtr)
#define load_gdt(dtr) native_load_gdt(dtr)
#define store_gdt(dtr) native_store_gdt(dtr)

/* The GDT this CPU runs on, read-only view: the same table here. */
static inline struct desc_struct *get_current_gdt_ro(void)
{
	struct desc_ptr gdt;

	native_store_gdt(&gdt);
	return (struct desc_struct *)gdt.address;
}
/* Linux maps the GDT read-only through the fixmap and must switch back to a
 * writable one after VMX; b1nix's GDT is always the writable one. */
static inline void load_fixmap_gdt(int cpu) { (void)cpu; }
void *cpu_entry_stack(int cpu);
#define get_current_gdt_rw() get_current_gdt_ro()

/* This CPU's task-state segment: its base, and the selector ltr loaded. */
unsigned long lkpi_x86_tss_base(int cpu);
static inline u16 native_store_tr(void)
{
	u16 tr;

	__asm__ volatile("str %0" : "=r"(tr));
	return tr;
}

static inline unsigned long get_desc_base(const struct desc_struct *desc)
{
	return (unsigned)(desc->base0 | ((desc->base1) << 16) | ((desc->base2) << 24));
}
static inline unsigned long get_desc_limit(const struct desc_struct *desc)
{
	return desc->limit0 | (desc->limit1 << 16);
}

/* After a VM exit the TSS limit is 0x67 whatever the descriptor says; b1nix
 * gives user tasks no I/O permission bitmap, so nothing past it is used and
 * there is nothing to refresh. */
static inline void invalidate_tss_limit(void) { }
static inline void refresh_tss_limit(void) { }

static inline void set_desc_base(struct desc_struct *desc, unsigned long base)
{
	desc->base0 = base & 0xffff;
	desc->base1 = (base >> 16) & 0xff;
	desc->base2 = (base >> 24) & 0xff;
}
static inline void set_desc_limit(struct desc_struct *desc, unsigned long limit)
{
	desc->limit0 = limit & 0xffff;
	desc->limit1 = (limit >> 16) & 0xf;
}

#endif
