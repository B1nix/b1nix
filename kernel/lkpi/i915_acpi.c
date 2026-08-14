/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The ACPI integration i915 expects, for a system that has ACPI but does not
 * expose the graphics _DSM methods to a driver.
 *
 * Upstream's drivers/gpu/drm/i915/display/intel_acpi.c is plain GPL-2.0 and is
 * therefore not imported (see tools/drm/fetch-i915.sh). Only its *header* comes
 * from the staged tree, which declares these under CONFIG_ACPI; this file
 * supplies the definitions.
 *
 * That option has to be on for a reason that has nothing to do with _DSM:
 * display/intel_opregion.c is gated on it, and the OpRegion is where the VBT
 * lives on a machine whose graphics BIOS did not leave one in the PCI ROM.
 * Without CONFIG_ACPI i915 uses the header's inline stubs, never looks for the
 * OpRegion at all, and synthesises a default VBT — which is what it was doing
 * here, with every port guessed rather than described.
 *
 * What is absent, and what it costs:
 *
 *   _DSM handler       — the ACPI method a driver registers so firmware can ask
 *                        it to switch displays or report panel state. b1nix
 *                        evaluates no graphics _DSM, so nothing would call it.
 *   BIOS data funcs    — a capability bitmask read from _DSM, used to decide
 *                        whether firmware owns brightness. Absent means i915
 *                        drives the panel itself, which is what we want here.
 *   device id update   — renumbers the ACPI display ids to match the ports the
 *                        VBT describes, for firmware that keys on them. No
 *                        firmware here reads them back.
 *   connector fwnodes  — links each connector to its ACPI device node so
 *                        userspace can match a panel to a firmware object.
 *                        b1nix's ACPI tables are not exposed that way.
 *   video register     — hands the backlight to the ACPI video driver when
 *                        firmware claims it. Nothing claims it here, so i915
 *                        keeps native backlight control.
 *
 * All five are platform integration, not display bring-up: skipping them leaves
 * the driver in the "firmware does not participate" configuration, which is the
 * configuration this machine is actually in.
 */

#include "intel_acpi.h"

void intel_register_dsm_handler(void)
{
}

void intel_unregister_dsm_handler(void)
{
}

void intel_dsm_get_bios_data_funcs_supported(struct drm_i915_private *i915)
{
	/* Leaves the capability mask zero: no firmware-owned functions. */
	(void)i915;
}

void intel_acpi_device_id_update(struct drm_i915_private *i915)
{
	(void)i915;
}

void intel_acpi_assign_connector_fwnodes(struct drm_i915_private *i915)
{
	(void)i915;
}

void intel_acpi_video_register(struct drm_i915_private *i915)
{
	(void)i915;
}
