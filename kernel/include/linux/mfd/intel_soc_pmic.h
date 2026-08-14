/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MFD_INTEL_SOC_PMIC_H
#define LKPI_LINUX_MFD_INTEL_SOC_PMIC_H
/* The SoC power-management IC that drives panel power on Bay Trail and Cherry
 * Trail. Not present on the Gen8/Gen9.5 desktop and mobile parts this cut
 * targets; see <linux/pwm.h> for the same reasoning. */
struct intel_soc_pmic { int dummy; };
#endif
