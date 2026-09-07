/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PSI_H
#define LKPI_LINUX_PSI_H

/*
 * Pressure stall information: how long tasks spent waiting on memory or I/O,
 * aggregated for userspace to read out of /proc/pressure. b1nix does not
 * account it.
 *
 * The two calls bracket a stall — a filesystem marks the window in which it is
 * waiting for a page — so both must exist and both do nothing. There is no
 * state between them to get wrong.
 */

static inline void psi_memstall_enter(unsigned long *flags) { (void)flags; }
static inline void psi_memstall_leave(unsigned long *flags) { (void)flags; }

#endif
