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
#include <lkpi/dma-mapping.h>
#include <lkpi/firmware.h>
#include <lkpi/idr.h>
#include <lkpi/io.h>
#include <lkpi/lock.h>
#include <lkpi/scatterlist.h>
#include <lkpi/workqueue.h>

/* M99 in-kernel self-test. Exercises every primitive above against
 * independently known values and emits M99-SMOKE markers. No-op outside
 * b1nix.test=1. */
void lkpi_selftest(void);

#endif
