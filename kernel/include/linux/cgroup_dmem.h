/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CGROUP_DMEM_H
#define LKPI_LINUX_CGROUP_DMEM_H
#include <linux/types.h>

/*
 * The device-memory cgroup controller (CONFIG_CGROUP_DMEM). b1nix's cgroups
 * have no dmem controller, so every charge succeeds against no limit: this is
 * upstream's own form for a kernel built without it.
 */
struct dmem_cgroup_region;
struct dmem_cgroup_pool_state;

static inline struct dmem_cgroup_region *
dmem_cgroup_register_region(u64 size, const char *name_fmt, ...)
{ (void)size; (void)name_fmt; return NULL; }
static inline void dmem_cgroup_unregister_region(struct dmem_cgroup_region *region)
{ (void)region; }
static inline int dmem_cgroup_try_charge(struct dmem_cgroup_region *region, u64 size,
                                         struct dmem_cgroup_pool_state **ret_pool,
                                         struct dmem_cgroup_pool_state **ret_limit_pool)
{
	(void)region; (void)size;
	*ret_pool = NULL;
	if (ret_limit_pool)
		*ret_limit_pool = NULL;
	return 0;
}
static inline void dmem_cgroup_uncharge(struct dmem_cgroup_pool_state *pool, u64 size)
{ (void)pool; (void)size; }
static inline bool
dmem_cgroup_state_evict_valuable(struct dmem_cgroup_pool_state *limit_pool,
                                 struct dmem_cgroup_pool_state *test_pool,
                                 bool ignore_low, bool *ret_hit_low)
{ (void)limit_pool; (void)test_pool; (void)ignore_low; (void)ret_hit_low; return true; }
static inline void dmem_cgroup_pool_state_put(struct dmem_cgroup_pool_state *pool)
{ (void)pool; }

#endif
