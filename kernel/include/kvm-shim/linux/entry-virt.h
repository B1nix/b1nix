/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_ENTRY_VIRT_H
#define LKPI_LINUX_ENTRY_VIRT_H
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <b1nix/kvm_bridge.h>

/*
 * Work to do before (re)entering a guest: a pending signal ends KVM_RUN with
 * -EINTR, a reschedule request yields the CPU. The same two things Linux's
 * generic entry code checks between guest entries. The reschedule request is
 * the scheduler's real one (a tick that found the vCPU not preemptible);
 * lkpi's general need_resched() answers "always", which here would keep the
 * vCPU from ever entering the guest.
 */
struct kvm_vcpu;

static inline bool __xfer_to_guest_mode_work_pending(void)
{
	return signal_pending(current) || b1nix_kvm_need_resched();
}

static inline bool xfer_to_guest_mode_work_pending(void)
{
	return __xfer_to_guest_mode_work_pending();
}

static inline void xfer_to_guest_mode_prepare(void) { }

/* 0 to go on, -EINTR when a signal is pending. */
static inline int xfer_to_guest_mode_handle_work(void)
{
	if (signal_pending(current))
		return -EINTR;
	if (b1nix_kvm_need_resched())
		cond_resched();
	return 0;
}
#endif
