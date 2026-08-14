/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_I915_KCONFIG_H
#define LKPI_I915_KCONFIG_H

/*
 * i915's Kconfig, as the compiler sees it.
 *
 * Upstream generates these from Kconfig into autoconf.h. b1nix has no Kconfig,
 * so they are written here — and the values are upstream's own defaults, copied
 * from drivers/gpu/drm/i915/Kconfig.profile rather than chosen. That matters
 * more than it looks: these are timeouts the hardware is tuned against, and a
 * number invented here would change behaviour that upstream considers part of
 * the driver.
 *
 * Force-included from the Makefile rather than included by the source, because
 * the source expects them to come from the build system and does not include
 * anything that would provide them.
 */

/*
 * The options that are OFF are not defined at all, deliberately.
 *
 * Upstream's headers test them with #ifdef, not with their value, so
 * `#define CONFIG_DRM_I915_PXP 0` reads as ON — the inline stubs disappear and
 * the build asks for object files that are only compiled when the option is
 * really set. An absent macro is the only spelling of "off" that both #ifdef
 * and #if agree on.
 *
 * Off here: CONFIG_DRM_I915_CAPTURE_ERROR, CONFIG_DRM_I915_COMPRESS_ERROR, CONFIG_DRM_I915_DEBUG, CONFIG_DRM_I915_DEBUG_GEM, CONFIG_DRM_I915_DEBUG_GEM_ONCE, CONFIG_DRM_I915_DEBUG_GUC, CONFIG_DRM_I915_DEBUG_MMIO, CONFIG_DRM_I915_DEBUG_RUNTIME_PM, CONFIG_DRM_I915_DEBUG_VBLANK_EVADE, CONFIG_DRM_I915_ERRLOG_GEM, CONFIG_DRM_I915_GVT, CONFIG_DRM_I915_PXP, CONFIG_DRM_I915_SELFTEST, CONFIG_DRM_I915_SELFTESTS, CONFIG_DRM_I915_SELFTEST_BROKEN, CONFIG_DRM_I915_SW_FENCE_CHECK_DAG, CONFIG_DRM_I915_SW_FENCE_DEBUG_OBJECTS, CONFIG_DRM_I915_TRACE_GEM, CONFIG_DRM_I915_TRACE_GTT.
 */

/*
 * ACPI. On, and the reason is the OpRegion rather than ACPI itself:
 * display/intel_opregion.c is gated on this option, and on a machine whose
 * graphics BIOS left no VBT in the PCI ROM the OpRegion is the only place the
 * port and panel description exists. With it off, i915 never looks, and
 * synthesises a default VBT with every port guessed.
 *
 * The _DSM half of the option — display/intel_acpi.c — is plain GPL-2.0 and is
 * not imported; kernel/lkpi/i915_acpi.c supplies those entry points instead,
 * and says there what their absence costs.
 */
#define CONFIG_ACPI 1

/* Timeouts, in milliseconds unless the comment says otherwise. Upstream's
 * Kconfig.profile defaults, unchanged. */
#define CONFIG_DRM_I915_REQUEST_TIMEOUT        20000
#define CONFIG_DRM_I915_FENCE_TIMEOUT          10000
#define CONFIG_DRM_I915_USERFAULT_AUTOSUSPEND  250
#define CONFIG_DRM_I915_HEARTBEAT_INTERVAL     2500
#define CONFIG_DRM_I915_PREEMPT_TIMEOUT        640
#define CONFIG_DRM_I915_PREEMPT_TIMEOUT_COMPUTE 7500
#define CONFIG_DRM_I915_MAX_REQUEST_BUSYWAIT   8000 /* nanoseconds */
#define CONFIG_DRM_I915_STOP_TIMEOUT           100
#define CONFIG_DRM_I915_TIMESLICE_DURATION     1

/*
 * The features that are off.
 *
 * Each is off for a reason, not to make the build shorter:
 *
 *   GVT      — SR-IOV mediated passthrough; needs vfio/mdev, which b1nix has no
 *              equivalent of.
 *   PXP      — protected content, which needs the Management Engine.
 *   CAPTURE_ERROR / COMPRESS_ERROR — the GPU error state dump; it pulls in
 *              relay and zlib, and it is diagnostics rather than function.
 *   SELFTEST — upstream's own test harness, which is GPL-2.0 in places and
 *              tests hardware this cut does not claim to drive yet.
 *   the DEBUG_* set — extra assertions that also enable code paths using
 *              facilities b1nix lacks (debugobjects, fault injection).
 *
 * Spelled as `#define ... 0` rather than left undefined because the source
 * tests them with IS_ENABLED(), which requires the symbol to exist.
 */

/* Which PCI IDs to bind to without being asked. Empty: this driver is bound
 * deliberately, not by probing everything it recognises. */
#define CONFIG_DRM_I915_FORCE_PROBE ""


/*
 * Headers upstream's include graph reaches and ours does not.
 *
 * An imported .c file includes what it needs *directly*; everything else it
 * uses arrives transitively through Linux's own headers. b1nix's shims are
 * flatter — a shim declares what it is about and stops — so a handful of
 * things (framebuffer console states, CPU feature data, page-cache flags,
 * module parameters) are declared in a header nothing in the chain includes.
 *
 * Adding those includes to b1nix's own headers would reshape its include graph
 * to match Linux's, cycles and all: <linux/wait.h> reaching <linux/bitops.h>
 * through <linux/types.h> already cost one round of "clear_bit implicitly
 * declared". Here instead, in the file that is force-included into every i915
 * translation unit and nothing else, where it affects no other imported driver
 * and no b1nix source.
 */
#include <linux/bitmap.h>
#include <linux/capability.h>
#include <linux/debugobjects.h>
#include <linux/dma-fence-array.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/jump_label.h>
#include <linux/limits.h>
#include <linux/mman.h>
#include <linux/moduleparam.h>
#include <linux/pagemap.h>
#include <linux/processor.h>
#include <linux/relay.h>
#include <linux/sysctl.h>
#include <linux/vmalloc.h>
#include <video/mipi_display.h>


#include <linux/smp.h>
#include <linux/swap.h>
#include <linux/writeback.h>

#endif
