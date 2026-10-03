/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_KCONFIG_H
#define KVM_SHIM_ARM64_LINUX_KCONFIG_H
/* In assembly only the IS_ENABLED() machinery, which <kvm_kconfig.h> has;
 * linuxkpi's C header brings in <linux/kernel.h> with it. */
#ifndef __ASSEMBLY__
#include_next <linux/kconfig.h>
#endif
#endif
