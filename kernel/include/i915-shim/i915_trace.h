/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_I915_TRACE_H
#define LKPI_I915_TRACE_H

/*
 * i915's tracepoints, as nothing.
 *
 * Upstream's i915_trace.h is plain GPL-2.0 — the only part of i915 that is,
 * along with the file that instantiates it — so it is not imported and this
 * stands in its place. b1nix has no ftrace, so upstream's header would expand
 * to nothing here anyway; what is lost is the tracing, not any behaviour.
 *
 * A variadic macro per call site rather than one blanket definition: the names
 * are what the imported source calls, and spelling them out means a tracepoint
 * that appears in a future rebase fails to compile rather than being silently
 * swallowed by a catch-all.
 */

/* See the note in display/intel_display_trace.h: upstream's header includes
 * these first and callers depend on the transitive reach. */

#include <drm/drm_drv.h>
#include "gt/intel_engine.h"
#include "i915_drv.h"

#define TRACE_NOOP(...) do { } while (0)

#define trace_i915_context_create(...)            TRACE_NOOP()
#define trace_i915_context_create_enabled()       0
#define trace_i915_context_free(...)              TRACE_NOOP()
#define trace_i915_context_free_enabled()         0
#define trace_i915_gem_evict(...)                 TRACE_NOOP()
#define trace_i915_gem_evict_enabled()            0
#define trace_i915_gem_evict_node(...)            TRACE_NOOP()
#define trace_i915_gem_evict_node_enabled()       0
#define trace_i915_gem_evict_vm(...)              TRACE_NOOP()
#define trace_i915_gem_evict_vm_enabled()         0
#define trace_i915_gem_object_clflush(...)        TRACE_NOOP()
#define trace_i915_gem_object_clflush_enabled()   0
#define trace_i915_gem_object_create(...)         TRACE_NOOP()
#define trace_i915_gem_object_create_enabled()    0
#define trace_i915_gem_object_destroy(...)        TRACE_NOOP()
#define trace_i915_gem_object_destroy_enabled()   0
#define trace_i915_gem_object_fault(...)          TRACE_NOOP()
#define trace_i915_gem_object_fault_enabled()     0
#define trace_i915_gem_object_pread(...)          TRACE_NOOP()
#define trace_i915_gem_object_pread_enabled()     0
#define trace_i915_gem_object_pwrite(...)         TRACE_NOOP()
#define trace_i915_gem_object_pwrite_enabled()    0
#define trace_i915_gem_shrink(...)                TRACE_NOOP()
#define trace_i915_gem_shrink_enabled()           0
#define trace_i915_ppgtt_create(...)              TRACE_NOOP()
#define trace_i915_ppgtt_create_enabled()         0
#define trace_i915_ppgtt_release(...)             TRACE_NOOP()
#define trace_i915_ppgtt_release_enabled()        0
#define trace_i915_request_add(...)               TRACE_NOOP()
#define trace_i915_request_add_enabled()          0
#define trace_i915_request_execute(...)           TRACE_NOOP()
#define trace_i915_request_execute_enabled()      0
#define trace_i915_request_guc_submit(...)        TRACE_NOOP()
#define trace_i915_request_guc_submit_enabled()   0
#define trace_i915_request_in(...)                TRACE_NOOP()
#define trace_i915_request_in_enabled()           0
#define trace_i915_request_out(...)               TRACE_NOOP()
#define trace_i915_request_out_enabled()          0
#define trace_i915_request_queue(...)             TRACE_NOOP()
#define trace_i915_request_queue_enabled()        0
#define trace_i915_request_retire(...)            TRACE_NOOP()
#define trace_i915_request_retire_enabled()       0
#define trace_i915_request_submit(...)            TRACE_NOOP()
#define trace_i915_request_submit_enabled()       0
#define trace_i915_request_wait_begin(...)        TRACE_NOOP()
#define trace_i915_request_wait_begin_enabled()   0
#define trace_i915_request_wait_end(...)          TRACE_NOOP()
#define trace_i915_request_wait_end_enabled()     0
#define trace_i915_vma_bind(...)                  TRACE_NOOP()
#define trace_i915_vma_bind_enabled()             0
#define trace_i915_vma_unbind(...)                TRACE_NOOP()
#define trace_i915_vma_unbind_enabled()           0
#define trace_intel_context_ban(...)              TRACE_NOOP()
#define trace_intel_context_ban_enabled()         0
#define trace_intel_context_create(...)           TRACE_NOOP()
#define trace_intel_context_create_enabled()      0
#define trace_intel_context_deregister(...)       TRACE_NOOP()
#define trace_intel_context_deregister_enabled()  0
#define trace_intel_context_deregister_done(...)  TRACE_NOOP()
#define trace_intel_context_deregister_done_enabled() 0
#define trace_intel_context_do_pin(...)           TRACE_NOOP()
#define trace_intel_context_do_pin_enabled()      0
#define trace_intel_context_do_unpin(...)         TRACE_NOOP()
#define trace_intel_context_do_unpin_enabled()    0
#define trace_intel_context_fence_release(...)    TRACE_NOOP()
#define trace_intel_context_fence_release_enabled() 0
#define trace_intel_context_free(...)             TRACE_NOOP()
#define trace_intel_context_free_enabled()        0
#define trace_intel_context_register(...)         TRACE_NOOP()
#define trace_intel_context_register_enabled()    0
#define trace_intel_context_reset(...)            TRACE_NOOP()
#define trace_intel_context_reset_enabled()       0
#define trace_intel_context_sched_disable(...)    TRACE_NOOP()
#define trace_intel_context_sched_disable_enabled() 0
#define trace_intel_context_sched_done(...)       TRACE_NOOP()
#define trace_intel_context_sched_done_enabled()  0
#define trace_intel_context_sched_enable(...)     TRACE_NOOP()
#define trace_intel_context_sched_enable_enabled() 0
#define trace_intel_context_set_prio(...)         TRACE_NOOP()
#define trace_intel_context_set_prio_enabled()    0
#define trace_intel_context_steal_guc_id(...)     TRACE_NOOP()
#define trace_intel_context_steal_guc_id_enabled() 0

#endif
