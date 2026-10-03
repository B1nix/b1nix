/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MMU_NOTIFIER_H
#define LKPI_LINUX_MMU_NOTIFIER_H
#include <linux/types.h>
/*
 * Callbacks when userspace unmaps memory a driver has pinned.
 *
 * This is what makes userptr buffers safe — without it, a GEM object backed by
 * user pages keeps them after munmap. b1nix has no such notifier, so userptr is
 * not a path that can be enabled by defining these; it is a path that stays
 * off. The declarations exist so translation units that reference the type
 * compile, and any driver that registers one will fail to link rather than run
 * unsafely.
 */
struct mmu_notifier;
struct mmu_notifier_ops;
struct mmu_interval_notifier;
struct mm_struct;

/* A host TLB flush of [start, end) in @mm, passed on to a secondary TLB that
 * shares its page tables (the KVM glue, kernel/lkpi/kvm_mm.c). */
void mmu_notifier_arch_invalidate_secondary_tlbs(struct mm_struct *mm,
						 unsigned long start,
						 unsigned long end);
#endif
