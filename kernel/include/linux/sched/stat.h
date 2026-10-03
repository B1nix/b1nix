/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SCHED_STAT_H
#define LKPI_LINUX_SCHED_STAT_H
#include <linux/sched.h>
/* Is the current task the only runnable one on this CPU? KVM's halt polling
 * stops polling when something else wants the CPU. */
bool single_task_running(void);
unsigned int nr_running(void);
#endif
