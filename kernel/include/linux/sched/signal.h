/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SCHED_SIGNAL_H
#define LKPI_LINUX_SCHED_SIGNAL_H
#include <linux/sched.h>
/* Imported code checks for a pending signal to bail out of a long wait. b1nix
 * kernel threads running the DRM core are not signal targets, so this reports
 * "nothing pending" — true for every caller that exists today, and the day a
 * driver waits on behalf of a user task it gets a real answer instead. */
static inline int fatal_signal_pending(void *t) { (void)t; return 0; }
#endif
