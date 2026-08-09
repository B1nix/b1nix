/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_CAPABILITY_H
#define LKPI_LINUX_CAPABILITY_H
#include <linux/types.h>
/* Capability checks on the calling task. b1nix has real capabilities (M40), but
 * the DRM core asks about the task that issued an ioctl, and no ioctl path is
 * wired yet — so this reports "not privileged", the safe answer: a driver that
 * believes it lacks a capability declines rather than proceeds. */
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
static inline bool capable(int cap) { (void)cap; return false; }
#endif
