/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_LINUX_MM_TYPES_H
#define KVM_SHIM_LINUX_MM_TYPES_H
#include_next <linux/mm_types.h>
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/list.h>

/*
 * A b1nix process address space, as KVM holds on to it (M131).
 *
 * lkpi elsewhere never needed more than the name. KVM keeps the owner's mm
 * for the life of a VM: it walks its page tables, faults its pages in, and
 * registers MMU notifiers on it, which b1nix's memory manager calls when a
 * mapping in that space changes. One mm per address space (the PML4 is the
 * identity), shared by the process's threads; kernel/lkpi/kvm_mm.c owns
 * them.
 */
struct mm_struct {
	u64 pml4_phys;           /* the b1nix address space this stands for */
	struct rw_semaphore mmap_lock;
	atomic_t mm_users;       /* the address space is alive */
	atomic_t mm_count;       /* this struct is alive */
	struct hlist_head notifiers;
	spinlock_t notifier_lock;
	struct list_head node;   /* lkpi's registry */
};
#endif
