/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_LINUX_BITS_H
#define KVM_SHIM_LINUX_BITS_H
/* Bit helpers usable from assembly too: msr-index.h is included by KVM's
 * entry code (vmx/vmenter.S) for the MSR numbers it names. */
#ifdef __ASSEMBLY__
#include <linux/const.h>
#define BIT(nr)       (1 << (nr))
#define BIT_ULL(nr)   (1 << (nr))
#define GENMASK(h, l) (((~0) << (l)) & (~0 >> (63 - (h))))
#define GENMASK_ULL(h, l) GENMASK(h, l)
#else
#include_next <linux/bits.h>
#endif
#endif
