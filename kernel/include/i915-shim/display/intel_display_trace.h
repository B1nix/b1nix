/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_INTEL_DISPLAY_TRACE_H
#define LKPI_INTEL_DISPLAY_TRACE_H
/* The display half of i915's tracepoints. Same reasoning as <i915_trace.h>:
 * upstream's is plain GPL-2.0 and is not imported, and ftrace does not exist
 * here, so these expand to nothing. */

/* Upstream's header pulls these in before defining its tracepoints, and the
 * display sources rely on that: several .c files reach to_i915() and
 * to_intel_crtc() through this include and nothing else. Keeping the same
 * transitive set is part of standing in for it. */

#include <linux/string.h>
#include <linux/string_helpers.h>
#include "intel_crtc.h"
#include "intel_display_core.h"
#include "intel_display_limits.h"
#include "intel_display_types.h"
#include "intel_vblank.h"

#define TRACE_DISPLAY_NOOP(...) do { } while (0)

#define trace_g4x_wm(...)                           TRACE_DISPLAY_NOOP()
#define trace_g4x_wm_enabled()                      0
#define trace_intel_cpu_fifo_underrun(...)          TRACE_DISPLAY_NOOP()
#define trace_intel_cpu_fifo_underrun_enabled()     0
#define trace_intel_crtc_flip_done(...)             TRACE_DISPLAY_NOOP()
#define trace_intel_crtc_flip_done_enabled()        0
#define trace_intel_crtc_vblank_work_end(...)       TRACE_DISPLAY_NOOP()
#define trace_intel_crtc_vblank_work_end_enabled()  0
#define trace_intel_crtc_vblank_work_start(...)     TRACE_DISPLAY_NOOP()
#define trace_intel_crtc_vblank_work_start_enabled() 0
#define trace_intel_fbc_activate(...)               TRACE_DISPLAY_NOOP()
#define trace_intel_fbc_activate_enabled()          0
#define trace_intel_fbc_deactivate(...)             TRACE_DISPLAY_NOOP()
#define trace_intel_fbc_deactivate_enabled()        0
#define trace_intel_fbc_nuke(...)                   TRACE_DISPLAY_NOOP()
#define trace_intel_fbc_nuke_enabled()              0
#define trace_intel_frontbuffer_flush(...)          TRACE_DISPLAY_NOOP()
#define trace_intel_frontbuffer_flush_enabled()     0
#define trace_intel_frontbuffer_invalidate(...)     TRACE_DISPLAY_NOOP()
#define trace_intel_frontbuffer_invalidate_enabled() 0
#define trace_intel_memory_cxsr(...)                TRACE_DISPLAY_NOOP()
#define trace_intel_memory_cxsr_enabled()           0
#define trace_intel_pch_fifo_underrun(...)          TRACE_DISPLAY_NOOP()
#define trace_intel_pch_fifo_underrun_enabled()     0
#define trace_intel_pipe_crc(...)                   TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_crc_enabled()              0
#define trace_intel_pipe_disable(...)               TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_disable_enabled()          0
#define trace_intel_pipe_enable(...)                TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_enable_enabled()           0
#define trace_intel_pipe_scaler_update_arm(...)     TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_scaler_update_arm_enabled() 0
#define trace_intel_pipe_update_end(...)            TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_update_end_enabled()       0
#define trace_intel_pipe_update_start(...)          TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_update_start_enabled()     0
#define trace_intel_pipe_update_vblank_evaded(...)  TRACE_DISPLAY_NOOP()
#define trace_intel_pipe_update_vblank_evaded_enabled() 0
#define trace_intel_plane_async_flip(...)           TRACE_DISPLAY_NOOP()
#define trace_intel_plane_async_flip_enabled()      0
#define trace_intel_plane_disable_arm(...)          TRACE_DISPLAY_NOOP()
#define trace_intel_plane_disable_arm_enabled()     0
#define trace_intel_plane_scaler_update_arm(...)    TRACE_DISPLAY_NOOP()
#define trace_intel_plane_scaler_update_arm_enabled() 0
#define trace_intel_plane_update_arm(...)           TRACE_DISPLAY_NOOP()
#define trace_intel_plane_update_arm_enabled()      0
#define trace_intel_plane_update_noarm(...)         TRACE_DISPLAY_NOOP()
#define trace_intel_plane_update_noarm_enabled()    0
#define trace_intel_scaler_disable_arm(...)         TRACE_DISPLAY_NOOP()
#define trace_intel_scaler_disable_arm_enabled()    0
#define trace_vlv_fifo_size(...)                    TRACE_DISPLAY_NOOP()
#define trace_vlv_fifo_size_enabled()               0
#define trace_vlv_wm(...)                           TRACE_DISPLAY_NOOP()
#define trace_vlv_wm_enabled()                      0

#endif
