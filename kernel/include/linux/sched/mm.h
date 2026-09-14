/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SCHED_MM_H
#define LKPI_LINUX_SCHED_MM_H

#include <linux/types.h>
#include <linux/gfp.h>

/*
 * Per-task allocation scope.
 *
 * `memalloc_nofs_save` says: for the rest of this region, every allocation is
 * implicitly GFP_NOFS, even the ones made by code that does not know it is
 * inside a filesystem. That is the point — a filesystem holding a transaction
 * calls into helpers that allocate, and each of them cannot be expected to know
 * to clear __GFP_FS.
 *
 * The paired restore takes the PREVIOUS value, not "off": these nest, and a
 * restore that unconditionally cleared the flag would re-enable reclaim
 * recursion in the middle of an outer scope. Every caller in the imported code
 * is written that way, so the interface has to be.
 *
 * b1nix's reclaim does not call into a filesystem today, so the scope records a
 * value that nothing yet reads. It is recorded rather than discarded because
 * the day reclaim does, this is where it looks — and a stub that returned a
 * constant would have quietly broken the nesting by then.
 */

unsigned int memalloc_nofs_save(void);
void memalloc_nofs_restore(unsigned int flags);
unsigned int memalloc_noio_save(void);
void memalloc_noio_restore(unsigned int flags);
unsigned int memalloc_nowait_save(void);
void memalloc_nowait_restore(unsigned int flags);

/* The allocation flags implied by the current scope, applied to a request. */
gfp_t current_gfp_context(gfp_t flags);

struct mm_struct;
static inline void mmgrab(struct mm_struct *mm) { (void)mm; }
static inline void mmdrop(struct mm_struct *mm) { (void)mm; }

#endif
