/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_CPU_DEVICE_ID_H
#define LKPI_ASM_CPU_DEVICE_ID_H
#include <linux/processor.h>
#include <asm/intel-family.h>

/* A table of CPUs by vendor/family/model, terminated by an all-zero entry;
 * x86_match_cpu() returns the entry the boot CPU matches, or NULL. */
struct x86_cpu_id {
	u16 vendor;
	u16 family;
	u16 model;
	u16 flags;             /* nonzero: a real entry, so the terminator is 0 */
	unsigned long driver_data;
};

#define X86_MATCH_VFM(vfm, data)                                  \
	{ .vendor = VFM_VENDOR(vfm), .family = VFM_FAMILY(vfm),   \
	  .model = VFM_MODEL(vfm), .flags = 1,                    \
	  .driver_data = (unsigned long)(data) }

static inline const struct x86_cpu_id *x86_match_cpu(const struct x86_cpu_id *match)
{
	const struct x86_cpu_id *m;

	for (m = match; m->flags; m++)
		if (m->vendor == boot_cpu_data.x86_vendor &&
		    m->family == boot_cpu_data.x86 &&
		    m->model == boot_cpu_data.x86_model)
			return m;
	return NULL;
}

#endif
