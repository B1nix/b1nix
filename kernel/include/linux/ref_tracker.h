/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_REF_TRACKER_H
#define LKPI_LINUX_REF_TRACKER_H
#include <linux/types.h>

/*
 * Reference-tracker debugging (CONFIG_REF_TRACKER) is not built. This is the
 * upstream form for that configuration: empty directories and trackers that
 * record nothing, so callers compile and pay nothing.
 */
struct ref_tracker;
struct ref_tracker_dir { };

static inline void ref_tracker_dir_init(struct ref_tracker_dir *dir,
					unsigned int quarantine_count,
					const char *class)
{ (void)dir; (void)quarantine_count; (void)class; }
static inline void ref_tracker_dir_exit(struct ref_tracker_dir *dir)
{ (void)dir; }
static inline void ref_tracker_dir_print(struct ref_tracker_dir *dir,
					 unsigned int display_limit)
{ (void)dir; (void)display_limit; }
static inline int ref_tracker_alloc(struct ref_tracker_dir *dir,
				    struct ref_tracker **trackerp, gfp_t gfp)
{ (void)dir; (void)trackerp; (void)gfp; return 0; }
static inline int ref_tracker_free(struct ref_tracker_dir *dir,
				   struct ref_tracker **trackerp)
{ (void)dir; (void)trackerp; return 0; }

static inline int ref_tracker_dir_snprint(struct ref_tracker_dir *dir, char *buf,
                                          size_t size)
{ (void)dir; (void)buf; (void)size; return 0; }

#endif
