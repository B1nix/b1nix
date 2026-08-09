/*
 * SPDX-License-Identifier: MIT
 *
 * M99 — linuxkpi: a Linux-shaped driver API implemented on b1nix primitives.
 *
 * Written from scratch. Linux's own headers under include/linux are GPL-2.0; the drivers
 * b1nix wants to carry (i915, amdgpu, the DRM core) are MIT. Copying the
 * headers would relicense the whole tree, so every declaration here is an
 * independent implementation of the same API *shape* — the names and signatures
 * a driver calls, backed by b1nix's kheap, scheduler, VFS and paging code.
 *
 * Scope rules for this layer:
 *   - No Linux source is copied, including comments and struct layouts that
 *     carry no functional meaning.
 *   - Nothing here may sleep while a b1nix spinlock is held (see CLAUDE.md);
 *     the sleeping primitives (completion, workqueue, request_firmware) are
 *     documented as such at each declaration.
 *   - Everything is a real implementation over an existing kernel service. This
 *     layer adds no new hardware assumptions.
 */

#ifndef LKPI_LKPI_H
#define LKPI_LKPI_H

#include <lkpi/types.h>
#include <lkpi/completion.h>
#include <lkpi/device.h>
#include <lkpi/dma-mapping.h>
#include <lkpi/firmware.h>
#include <lkpi/idr.h>
#include <lkpi/interval_tree.h>
#include <lkpi/io.h>
#include <lkpi/kref.h>
#include <lkpi/kthread_worker.h>
#include <lkpi/lock.h>
#include <lkpi/page.h>
#include <lkpi/rbtree.h>
#include <lkpi/rcu.h>
#include <lkpi/xarray.h>
#include <lkpi/scatterlist.h>
#include <lkpi/wait.h>
#include <lkpi/workqueue.h>
#include <lkpi/ww_mutex.h>

/* M99 in-kernel self-test. Exercises every primitive above against
 * independently known values and emits M99-SMOKE markers. No-op outside
 * b1nix.test=1. */
void lkpi_selftest(void);

/* M101 self-test: the primitives added for the DRM core — kref, wait queues,
 * wound/wait mutexes and red-black trees. Emits M101-SMOKE markers. Separate
 * from lkpi_selftest because these are a later milestone's surface and are
 * reported as such. No-op outside b1nix.test=1. */
void lkpi_selftest_m101(void);

/* M101: the RCU grace-period proof, split out because it has to run from the
 * early SMP self-test block — it needs a reader executing on another CPU at the
 * same moment as the writer, and a stealable worker picked up by a parked AP is
 * the only placement this kernel guarantees. No-op outside b1nix.test=1 or on a
 * single-CPU machine, where a reader and a writer cannot overlap at all. */
void lkpi_rcu_smp_selftest(void);

/* M101: proof that the imported DRM core runs, not merely links. Calls into
 * upstream's own allocator, rectangle maths and format table and checks the
 * answers against values computed independently. Emits M101-IMPORT markers.
 * No-op outside b1nix.test=1. */
void drm_import_selftest(void);

/* M101: bring the imported DRM core's own initcall up, once, before any device
 * registers with it. */
void drm_core_bringup(void);

/* M101: register a device with the imported core and render one frame through
 * it, all the way to the scanout. */
void drm_kms_selftest(void);

#endif
