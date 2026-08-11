/* SPDX-License-Identifier: MIT */
#ifndef LKPI_INTEL_DISPLAY_TRACE_H
#define LKPI_INTEL_DISPLAY_TRACE_H
/* The display half of i915's tracepoints. Same reasoning as <i915_trace.h>:
 * upstream's is plain GPL-2.0 and is not imported, and ftrace does not exist
 * here, so these expand to nothing. */

/* Upstream's header pulls these in before defining its tracepoints, and the
 * display sources rely on that: several .c files reach to_i915() and
 * to_intel_crtc() through this include and nothing else. Keeping the same
 * transitive set is part of standing in for it. */
#include "i915_drv.h"
#include "intel_crtc.h"
#include "intel_display_types.h"

#define TRACE_DISPLAY_NOOP(...) do { } while (0)

#define trace_intel_pipe_enable(...)             TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_disable(...)            TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_crc(...)                TRACE_DISPLAY_NOOP()
#define trace_intel_cpu_fifo_underrun(...)       TRACE_DISPLAY_NOOP()
#define trace_intel_pch_fifo_underrun(...)       TRACE_DISPLAY_NOOP()
#define trace_intel_memory_cxsr(...)             TRACE_DISPLAY_NOOP()
#define trace_g4x_wm(...)                        TRACE_DISPLAY_NOOP()
#define trace_vlv_wm(...)                        TRACE_DISPLAY_NOOP()
#define trace_vlv_fifo_size(...)                 TRACE_DISPLAY_NOOP()
#define trace_intel_plane_update_noarm(...)      TRACE_DISPLAY_NOOP()
#define trace_intel_plane_update_arm(...)        TRACE_DISPLAY_NOOP()
#define trace_intel_plane_disable_arm(...)       TRACE_DISPLAY_NOOP()
#define trace_intel_fbc_activate(...)            TRACE_DISPLAY_NOOP()
#define trace_intel_fbc_deactivate(...)          TRACE_DISPLAY_NOOP()
#define trace_intel_fbc_nuke(...)                TRACE_DISPLAY_NOOP()
#define trace_intel_crtc_vblank_work_start(...)  TRACE_DISPLAY_NOOP()
#define trace_intel_crtc_vblank_work_end(...)    TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_update_start(...)       TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_update_vblank_evaded(...) TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_update_end(...)         TRACE_DISPLAY_NOOP()
#define trace_intel_frontbuffer_invalidate(...)  TRACE_DISPLAY_NOOP()
#define trace_intel_frontbuffer_flush(...)       TRACE_DISPLAY_NOOP()
#endif
