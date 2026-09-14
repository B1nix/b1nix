/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_INTEL_FAMILY_H
#define LKPI_ASM_INTEL_FAMILY_H
#include <linux/processor.h>

/*
 * Intel CPU models, encoded as upstream encodes them: 8 bits each of vendor,
 * family and model. Only the models the imported code names are listed; the
 * values are CPUID facts (arch/x86/include/asm/intel-family.h).
 */
#define VFM_MODEL_BIT  0
#define VFM_FAMILY_BIT 8
#define VFM_VENDOR_BIT 16
#define VFM_MODEL(vfm)  (((vfm) >> VFM_MODEL_BIT) & 0xff)
#define VFM_FAMILY(vfm) (((vfm) >> VFM_FAMILY_BIT) & 0xff)
#define VFM_VENDOR(vfm) (((vfm) >> VFM_VENDOR_BIT) & 0xff)
#define VFM_MAKE(_vendor, _family, _model) \
	(((_model) << VFM_MODEL_BIT) | ((_family) << VFM_FAMILY_BIT) | ((_vendor) << VFM_VENDOR_BIT))
#define IFM(_fam, _model) VFM_MAKE(X86_VENDOR_INTEL, _fam, _model)

#define INTEL_KABYLAKE_L    IFM(6, 0x8E)
#define INTEL_KABYLAKE      IFM(6, 0x9E)
#define INTEL_COMETLAKE     IFM(6, 0xA5)
#define INTEL_ROCKETLAKE    IFM(6, 0xA7)
#define INTEL_ALDERLAKE     IFM(6, 0x97)
#define INTEL_ALDERLAKE_L   IFM(6, 0x9A)
#define INTEL_RAPTORLAKE    IFM(6, 0xB7)
#define INTEL_RAPTORLAKE_P  IFM(6, 0xBA)
#define INTEL_RAPTORLAKE_S  IFM(6, 0xBF)

#endif
