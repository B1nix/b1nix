/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_DCACHE_H
#define LKPI_LINUX_DCACHE_H

/*
 * The dentry, which is declared in <linux/fs.h> here along with the rest of the
 * VFS object model. Upstream splits them; this header exists because imported
 * code includes it by name, and pointing it at the definition is better than a
 * second one that can drift.
 */
#include <linux/fs.h>

/*
 * How much of a cache a shrinker should be willing to give up, as a fraction
 * of what it holds. Upstream scales this by sysctl_vfs_cache_pressure, whose
 * default is 100 — the identity. b1nix has no such knob, so the ratio is that
 * default rather than a number invented here.
 */
static inline unsigned long vfs_pressure_ratio(unsigned long val)
{ return val; }

#endif
