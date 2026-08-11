/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SHRINKER_H
#define LKPI_LINUX_SHRINKER_H
#include <linux/types.h>

/*
 * Memory the kernel may reclaim from a driver under pressure.
 *
 * A driver registers two callbacks: one that says how much it is holding that
 * could be freed, and one that is asked to free some of it. Linux walks the
 * registered shrinkers when it runs short.
 *
 * b1nix does not. Its allocator panics rather than reclaiming (see kmalloc's
 * contract), so nothing walks these and a registered shrinker is never called.
 * That is a real limitation and worth stating plainly: i915 caches GEM pages
 * expecting to be asked to give them back, and here it never will be — so the
 * machine runs out of memory where Linux would have shrunk the cache first.
 * Making the driver's own idle paths free more aggressively is the fix, not a
 * shrinker that lies about being connected to anything.
 */
struct shrink_control {
	gfp_t gfp_mask;
	unsigned long nr_to_scan;
	unsigned long nr_scanned;
	int nid;
};

struct shrinker {
	unsigned long (*count_objects)(struct shrinker *, struct shrink_control *);
	unsigned long (*scan_objects)(struct shrinker *, struct shrink_control *);
	long batch;
	int seeks;
	unsigned flags;
	void *private_data;
	const char *name;
};

#define DEFAULT_SEEKS 2
#define SHRINK_STOP (~0UL)
#define SHRINK_EMPTY (~0UL - 1)

/* Allocation and registration succeed so the driver's probe path completes;
 * nothing is recorded, because nothing would ever read it. */
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...);
void shrinker_register(struct shrinker *shrinker);
void shrinker_free(struct shrinker *shrinker);
static inline int register_shrinker(struct shrinker *s, const char *fmt, ...)
{ (void)s; (void)fmt; return 0; }
static inline void unregister_shrinker(struct shrinker *s) { (void)s; }

/* Wait for every running shrinker callback to finish, so a driver can free the
 * state they walk. b1nix drives shrinkers from a single reclaim thread and a
 * caller here is not that thread, so there is nothing in flight to wait for by
 * the time an unregister has completed. */
static inline void synchronize_shrinkers(void) { }

#endif
