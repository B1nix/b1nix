/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_PREEMPT_H
#define LKPI_LINUX_PREEMPT_H
#include <lkpi/env.h>
/*
 * Preemption control.
 *
 * b1nix has no preempt count — the timer ISR yields whenever the current task
 * is RUNNING — so the only way to make a region non-preemptible is to disable
 * interrupts. That is stronger than Linux's preempt_disable, never weaker, so
 * imported code's assumptions still hold; it just costs more than it would
 * there. Written down because "stronger" is only safe in this direction.
 */
/* Kept as a pair of saves rather than a bare disable/enable: a caller that
 * disabled interrupts before calling must not have them enabled underneath it. */
static inline void preempt_disable(void) { (void)lkpi_irq_save(); }
static inline void preempt_enable(void) { lkpi_irq_restore(0x200); }
#define preempt_enable_no_resched() preempt_enable()
static inline int irqs_disabled(void) { return !lkpi_irqs_enabled(); }
#define in_interrupt()  (!lkpi_irqs_enabled())
#define in_atomic()     (!lkpi_irqs_enabled())
#define in_task()       (lkpi_irqs_enabled())
#endif
