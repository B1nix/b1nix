/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_TRACE_EVENTS_IPI_H
#define LKPI_TRACE_EVENTS_IPI_H
#include <linux/tracepoint.h>
struct cpumask;
static inline void trace_ipi_send_cpumask(const struct cpumask *cpumask, unsigned long callsite, void *callback) { (void)cpumask; (void)callsite; (void)callback; }
static inline void trace_ipi_send_cpu(unsigned int cpu, unsigned long callsite, void *callback) { (void)cpu; (void)callsite; (void)callback; }
#endif
