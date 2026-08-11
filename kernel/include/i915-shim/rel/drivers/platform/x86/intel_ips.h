/* SPDX-License-Identifier: MIT */
#ifndef LKPI_INTEL_IPS_H
#define LKPI_INTEL_IPS_H
/*
 * intel_ips: the Ironlake platform's thermal/power controller, driven by a
 * separate module that i915 hands its GPU power readings to.
 *
 * The header lives outside drivers/gpu in upstream (drivers/platform/x86), and
 * gt/intel_rps.c includes it by a path relative to its own directory. Only that
 * relative path can find it, which is why this shim sits under a mirror of the
 * source tree rather than in kernel/include/linux — see the -I anchor in the
 * Makefile.
 *
 * All intel_rps.c wants from it is the symbol it pings on load, looked up with
 * symbol_get(). No ips driver exists here, so the lookup finds nothing and the
 * ping does not happen; on Ironlake that costs the hand-off between the two
 * drivers, and Ironlake is not an M102 target.
 */
void ips_link_to_i915_driver(void);
#endif
