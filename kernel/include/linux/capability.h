/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CAPABILITY_H
#define LKPI_LINUX_CAPABILITY_H
#include <linux/types.h>
/* Capability checks on the calling task, answered from b1nix's own credentials.
 *
 * This used to report "not privileged" unconditionally, on the reasoning that a
 * driver which believes it lacks a capability declines rather than proceeds.
 * That is the safe answer for a driver deciding what to attempt on its own
 * behalf, and the wrong one for an ioctl arriving from ring 3: upstream's
 * drm_master_check_perm() asks capable(CAP_SYS_ADMIN) before granting the DRM
 * master lease, so DRM_IOCTL_SET_MASTER answered EACCES to root. A compositor
 * cannot modeset without that lease, and kwin reports the whole failure as
 * "failed to open drm device" -- naming the one step that had worked. */
/*
 * Guarded because a bridge file — one that includes both this and b1nix's own
 * <b1nix/uidgid.h> — would otherwise redefine them. b1nix's numbering is the
 * authority for anything that reaches its capability checks; these values exist
 * only so imported code compiles, and capable() below always answers "no"
 * regardless of which constant it was handed.
 */
#ifndef CAP_SYS_ADMIN
#define CAP_SYS_ADMIN 21
#endif
#ifndef CAP_SYS_RAWIO
#define CAP_SYS_RAWIO 17
#endif
int lkpi_capable(int cap);
static inline bool capable(int cap) { return lkpi_capable(cap) != 0; }

/* Permission to read performance counters. b1nix has no perf interface for a
 * driver to expose (see <linux/perf_event.h>), so nothing is gated on this and
 * the answer is the restrictive one. */
static inline bool perfmon_capable(void) { return false; }


#ifndef CAP_SYS_NICE
#define CAP_SYS_NICE 23
#endif

#endif
