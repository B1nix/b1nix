/* SPDX-License-Identifier: MIT */
#ifndef LKPI_TRACE_EVENTS_DMA_FENCE_H
#define LKPI_TRACE_EVENTS_DMA_FENCE_H
#include <linux/tracepoint.h>
/* The dma-fence tracepoints. b1nix has no ftrace, so <linux/tracepoint.h>
 * expands every trace_* call to nothing; this header exists so the include
 * resolves and the call sites keep their shape. */
#define trace_dma_fence_emit(f)         do { (void)(f); } while (0)
#define trace_dma_fence_init(f)         do { (void)(f); } while (0)
#define trace_dma_fence_destroy(f)      do { (void)(f); } while (0)
#define trace_dma_fence_enable_signal(f) do { (void)(f); } while (0)
#define trace_dma_fence_signaled(f)     do { (void)(f); } while (0)
#define trace_dma_fence_wait_start(f)   do { (void)(f); } while (0)
#define trace_dma_fence_wait_end(f)     do { (void)(f); } while (0)
#endif
