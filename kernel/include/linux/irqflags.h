/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_IRQFLAGS_H
#define LKPI_LINUX_IRQFLAGS_H
#include <linux/spinlock.h>
#include <linux/preempt.h>

/* No lockdep, no irq tracing: the raw forms are the plain ones. */
#define raw_local_irq_disable()      local_irq_disable()
#define raw_local_irq_enable()       local_irq_enable()
#define raw_local_irq_save(flags)    local_irq_save(flags)
#define raw_local_irq_restore(flags) local_irq_restore(flags)
#define raw_irqs_disabled()          irqs_disabled()
#define lockdep_hardirqs_on(ip)      do { (void)(ip); } while (0)
#define lockdep_hardirqs_off(ip)     do { (void)(ip); } while (0)
#define lockdep_hardirqs_on_prepare() do { } while (0)
#define trace_hardirqs_on()          do { } while (0)
#define trace_hardirqs_off()         do { } while (0)
#define trace_hardirqs_on_prepare()  do { } while (0)
#endif
