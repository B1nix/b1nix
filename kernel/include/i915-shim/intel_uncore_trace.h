/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_INTEL_UNCORE_TRACE_H
#define LKPI_INTEL_UNCORE_TRACE_H

/*
 * The MMIO read/write tracepoint, as nothing. Upstream's header is plain
 * GPL-2.0 ftrace plumbing and is not staged (see tools/import/drm/fetch-i915.sh); the
 * register access it wraps runs either way.
 */
#include "i915_reg_defs.h"

#define trace_i915_reg_rw(...) do { } while (0)

#endif
