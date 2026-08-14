/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SUSPEND_H
#define LKPI_LINUX_SUSPEND_H
#include <linux/pm.h>
/* Whole-machine suspend states. b1nix implements none of them, so a driver
 * asking whether one is in progress is always told no — which is true. */
static inline bool pm_suspend_target_state_is_mem(void) { return false; }
#define PM_SUSPEND_ON  0
#define PM_SUSPEND_MEM 3
extern int pm_suspend_target_state;

/* The state a suspend targets. b1nix implements none, so the only value that
 * ever appears is PM_SUSPEND_ON. */
typedef int suspend_state_t;


#define PM_SUSPEND_TO_IDLE 1
#define PM_SUSPEND_STANDBY 2

#endif
