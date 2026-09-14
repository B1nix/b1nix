/* SPDX-License-Identifier: MIT */
/*
 * The AGP GMCH aperture, which b1nix does not drive.
 *
 * i915 reaches the graphics translation table two ways. Everything from Gen6
 * on programs it directly, which is what this kernel runs; Gen2 to Gen5 go
 * through the chipset's AGP bridge, whose driver is drivers/char/agp/intel-gtt.c
 * — a subsystem of its own, for hardware from 2004 to 2010, that nothing here
 * has.
 *
 * So the entry points exist and refuse. gt/intel_ggtt_gmch.c is imported
 * unchanged and calls them; the probe returns failure, and i915 declines the
 * old device at initialisation rather than running with a translation table
 * nobody is programming. The alternative — leaving them undefined — would take
 * the whole file out of the build, which is what pretending CONFIG_X86 was off
 * used to do, and that answer was wrong about more than this.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <drm/intel/intel-gtt.h>

void intel_gmch_gtt_get(u64 *gtt_total, phys_addr_t *mappable_base,
                        resource_size_t *mappable_end)
{
	if (gtt_total)
		*gtt_total = 0;
	if (mappable_base)
		*mappable_base = 0;
	if (mappable_end)
		*mappable_end = 0;
}

int intel_gmch_probe(struct pci_dev *bridge_pdev, struct pci_dev *gpu_pdev,
                     struct agp_bridge_data *bridge)
{
	(void)bridge_pdev; (void)gpu_pdev; (void)bridge;
	/* Zero is failure here: intel_ggtt_gmch_probe treats a false return as
	 * "no GMCH" and reports -EIO to its caller. */
	return 0;
}

void intel_gmch_remove(void) { }

bool intel_gmch_enable_gtt(void) { return false; }

void intel_gmch_gtt_flush(void) { }

void intel_gmch_gtt_insert_page(dma_addr_t addr, unsigned int pg,
                                unsigned int flags)
{ (void)addr; (void)pg; (void)flags; }

void intel_gmch_gtt_insert_sg_entries(struct sg_table *st, unsigned int pg_start,
                                      unsigned int flags)
{ (void)st; (void)pg_start; (void)flags; }

void intel_gmch_gtt_clear_range(unsigned int first_entry,
                                unsigned int num_entries)
{ (void)first_entry; (void)num_entries; }

/* No GMCH GTT exists (intel_gmch_probe reports none), so there is no entry
 * to read: not present. */
dma_addr_t intel_gmch_gtt_read_entry(unsigned int pg, bool *is_present,
                                     bool *is_local)
{
	(void)pg;
	if (is_present)
		*is_present = false;
	if (is_local)
		*is_local = false;
	return 0;
}
