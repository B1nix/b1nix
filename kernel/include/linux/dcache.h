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

/*
 * Name hashing for filesystems with their own d_hash (<linux/stringhash.h>
 * upstream). The value only has to be consistent within this kernel: it keys
 * in-memory lookups and is never stored. `salt` is the parent dentry upstream
 * mixes in; mixing it in here as well keeps equal names in different
 * directories apart.
 */
#define init_name_hash(salt) ((unsigned long)(salt))
static inline unsigned long partial_name_hash(unsigned long c,
                                              unsigned long prevhash)
{
	return (prevhash + (c << 4) + (c >> 4)) * 11;
}
static inline unsigned int end_name_hash(unsigned long hash)
{
	return (unsigned int)(hash ^ (hash >> 32));
}
static inline unsigned int full_name_hash(const void *salt, const char *name,
                                          unsigned int len)
{
	unsigned long hash = init_name_hash(salt);

	while (len--)
		hash = partial_name_hash((unsigned char)*name++, hash);
	return end_name_hash(hash);
}

#endif
