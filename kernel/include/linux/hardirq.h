/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_HARDIRQ_H
#define LKPI_LINUX_HARDIRQ_H
#include <linux/preempt.h>
#include <linux/irqflags.h>
#define in_irq()         in_interrupt()
#define in_hardirq()     in_interrupt()
#define in_nmi()         0
#define in_softirq()     0
#define irq_enter()      do { } while (0)
#define irq_exit()       do { } while (0)
#define irq_enter_rcu()  do { } while (0)
#define irq_exit_rcu()   do { } while (0)
#endif
