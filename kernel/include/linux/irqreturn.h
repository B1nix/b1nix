/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_IRQRETURN_H
#define LKPI_LINUX_IRQRETURN_H
/* What an interrupt handler tells the core: whether the interrupt was its. */
typedef enum irqreturn {
	IRQ_NONE = 0,
	IRQ_HANDLED = 1,
	IRQ_WAKE_THREAD = 2,
} irqreturn_t;
#endif
