/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_STACKDEPOT_H
#define LKPI_LINUX_STACKDEPOT_H
#include <linux/types.h>
/* Deduplicated stack-trace storage, used by DRM only to record where a
 * reference was taken when its leak tracking is enabled. b1nix does not build
 * that tracking, so a handle is always zero and no trace is stored — the
 * feature is off, not faked. */
typedef u32 depot_stack_handle_t;
static inline depot_stack_handle_t stack_depot_save(unsigned long *entries,
                                                    unsigned int nr, gfp_t gfp)
{ (void)entries; (void)nr; (void)gfp; return 0; }
static inline unsigned int stack_depot_fetch(depot_stack_handle_t h,
                                             unsigned long **entries)
{ (void)h; if (entries) *entries = 0; return 0; }
static inline void stack_depot_print(depot_stack_handle_t h) { (void)h; }
static inline int stack_depot_snprint(depot_stack_handle_t h, char *buf,
                                      usize size, int spaces)
{ (void)h; (void)spaces; if (buf && size) buf[0] = 0; return 0; }
static inline void stack_depot_init(void) { }
#endif
