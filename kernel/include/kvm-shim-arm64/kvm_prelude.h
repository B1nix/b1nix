/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_PRELUDE_H
#define KVM_SHIM_ARM64_PRELUDE_H
#include <linux/errno.h>
#include <linux/linkage.h>
#include <linux/percpu.h>
#include <linux/hrtimer.h>
#include <linux/cpumask.h>
#include <linux/file.h>
/* Linux reaches the page-order counts through <linux/gfp.h>; lkpi's does
 * not include the zone header, and protected KVM's allocator sizes by them. */
#include <linux/mmzone.h>
/* Arch headers Linux's generic ones bring in: the EL2 register layout through
 * <linux/hardirq.h>, the generic timer through <linux/timex.h>. */
#include <asm/kvm_arm.h>
#include <asm/arch_timer.h>
/* And what arm64 KVM's files reach through Linux's wider generic headers
 * (<linux/sched.h>, <linux/kvm_host.h>, <linux/mm.h>). */
#include <linux/sched.h>
#include <linux/thread_info.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/uuid.h>
#include <linux/timekeeping.h>
#include <linux/stacktrace.h>
#include <linux/hugetlb.h>
#include <linux/hw_breakpoint.h>
#include <linux/memblock.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <asm/mte.h>
#include <asm/smp_plat.h>
/* An invalid host virtual address, for KVM's memslot lookups. Linux's
 * generic answer is "anything from PAGE_OFFSET up", the kernel half; b1nix's
 * aarch64 kernel lives in the low half beside user space (its direct map is
 * the identity), so the bad range is everything past user space -- 48-bit
 * user addresses, kernel/arch/aarch64/signal.c's USER_ADDR_LIMIT. */
#define KVM_HVA_ERR_BAD		(1UL << 48)
#define KVM_HVA_ERR_RO_BAD	(KVM_HVA_ERR_BAD + PAGE_SIZE)
static inline bool kvm_is_error_hva(unsigned long addr)
{
	return addr >= KVM_HVA_ERR_BAD;
}
#endif
