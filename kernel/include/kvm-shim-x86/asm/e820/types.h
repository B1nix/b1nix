/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_E820_TYPES_H
#define KVM_SHIM_ASM_E820_TYPES_H
/* The firmware memory map's region types; b1nix's boot memory map is the
 * source (kernel/lkpi/kvm_mm.c answers e820__mapped_raw_any from it). */
#include <linux/types.h>
enum e820_type {
	E820_TYPE_RAM = 1,
	E820_TYPE_RESERVED = 2,
	E820_TYPE_ACPI = 3,
	E820_TYPE_NVS = 4,
	E820_TYPE_UNUSABLE = 5,
	E820_TYPE_PMEM = 7,
	E820_TYPE_PRAM = 12,
	E820_TYPE_SOFT_RESERVED = 0xefffffff,
	E820_TYPE_RESERVED_KERN = 128,
};
/* The legacy ISA hole, 640 KiB to 1 MiB. */
#define ISA_START_ADDRESS 0x000a0000
#define ISA_END_ADDRESS   0x00100000
#endif
