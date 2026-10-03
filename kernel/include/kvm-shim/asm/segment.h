/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_SEGMENT_H
#define KVM_SHIM_ASM_SEGMENT_H
/* Segment selectors as b1nix's GDT has them (kernel/arch/x86_64/boot.S):
 * kernel code 0x08, kernel data 0x10, user data 0x18, user code 0x20, then a
 * 16-byte TSS descriptor per CPU from 0x28. KVM writes the kernel ones into
 * every VMCS's host-state area, so they must be b1nix's, not Linux's. */
#define SEGMENT_RPL_MASK 0x3
#define SEGMENT_TI_MASK  0x4
#define SEGMENT_LDT      0x4
#define SEGMENT_GDT      0x0
#define GDT_ENTRY_KERNEL_CS 1
#define GDT_ENTRY_KERNEL_DS 2
#define GDT_ENTRY_TSS       5
#define __KERNEL_CS 0x08
#define __KERNEL_DS 0x10
#define __USER_DS   0x1b
#define __USER_CS   0x23
#ifndef __ASSEMBLY__
#define savesegment(seg, value) \
	__asm__("mov %%" #seg ",%k0" : "=r"(value) : : "memory")
#define loadsegment(seg, value) \
	__asm__ volatile("mov %k0, %%" #seg : : "r"(value) : "memory")
#endif
#endif
