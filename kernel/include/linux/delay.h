/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_DELAY_H
#define LKPI_LINUX_DELAY_H
#include <lkpi/env.h>
/* udelay busy-waits and may be called with interrupts off; msleep parks and may
 * not. Keeping them different is the point — a driver that sleeps in an atomic
 * section is a bug, and mapping both onto the same primitive would hide it. */
static inline void udelay(unsigned long usecs) { lkpi_udelay(usecs); }
static inline void ndelay(unsigned long ns) { udelay((ns + 999) / 1000); }
static inline void mdelay(unsigned long ms) { udelay(ms * 1000); }
static inline void msleep(unsigned int ms) { lkpi_sleep_jiffies((ms + 9) / 10); }
static inline void usleep_range(unsigned long lo, unsigned long hi)
{ (void)hi; udelay(lo); }

/* Sleeping for a number of milliseconds, interruptibly. b1nix kernel threads
 * are not signal targets, so nothing cuts it short and the return is always 0 —
 * the "slept the whole time" answer, which is the one callers act on. */
static inline unsigned long msleep_interruptible(unsigned int msecs)
{ msleep(msecs); return 0; }

#endif
