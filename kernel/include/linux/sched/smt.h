/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SCHED_SMT_H
#define LKPI_LINUX_SCHED_SMT_H
#include <linux/types.h>
/* Whether sibling hyperthreads are online. The scheduler has no notion of
 * them; lkpi answers from CPUID's threads-per-core. */
bool sched_smt_active(void);
#endif
