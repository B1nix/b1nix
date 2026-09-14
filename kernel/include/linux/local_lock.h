/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_LOCAL_LOCK_H
#define LKPI_LINUX_LOCAL_LOCK_H
#include <linux/spinlock.h>

/*
 * Per-CPU local locks.
 *
 * Upstream a local lock only pins the task to its CPU (preemption or interrupts
 * off), because the data it guards is per-CPU and nobody else can reach it.
 * Here DEFINE_PER_CPU is ONE instance shared by every CPU (<linux/percpu.h>),
 * so pinning is not enough: two CPUs would share the "local" data. A local
 * lock is therefore a real spinlock, which gives the exclusion the shared
 * instance needs and — spinlocks disabling interrupts here — the pinning too.
 */
typedef spinlock_t local_lock_t;

#define INIT_LOCAL_LOCK(name)  { 0 }
#define local_lock_init(l)     spin_lock_init(l)
#define local_lock(l)          spin_lock(this_cpu_ptr(l))
#define local_unlock(l)        spin_unlock(this_cpu_ptr(l))
#define local_lock_irq(l)      spin_lock(this_cpu_ptr(l))
#define local_unlock_irq(l)    spin_unlock(this_cpu_ptr(l))
#define local_lock_irqsave(l, f)      spin_lock_irqsave(this_cpu_ptr(l), f)
#define local_unlock_irqrestore(l, f) spin_unlock_irqrestore(this_cpu_ptr(l), f)

#endif
