/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CONTEXT_TRACKING_H
#define LKPI_LINUX_CONTEXT_TRACKING_H
/* No context tracking (no NOHZ_FULL, no RCU extended quiescent states): a
 * guest's time is not treated differently from the host's. */
static inline bool context_tracking_guest_enter(void) { return false; }
static inline bool context_tracking_guest_exit(void) { return false; }
static inline bool context_tracking_enabled(void) { return false; }
static inline bool context_tracking_enabled_this_cpu(void) { return false; }
#define ct_state() 0
#endif
