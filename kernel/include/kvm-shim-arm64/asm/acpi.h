/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_ACPI_H
#define KVM_SHIM_ARM64_ASM_ACPI_H
/*
 * No ACPI on the arm64 KVM path here: the GIC and timer come from the device
 * tree, and there is no APEI firmware-first error handling to claim a
 * synchronous external abort -- Linux's !CONFIG_ACPI_APEI_GHES answer, which
 * leaves the abort to KVM's own handling.
 */
#include <linux/errno.h>
struct pt_regs;
static inline int apei_claim_sea(struct pt_regs *regs) { (void)regs; return -ENOENT; }
#endif
