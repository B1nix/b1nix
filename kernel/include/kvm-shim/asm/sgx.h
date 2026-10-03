/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_SGX_H
#define KVM_SHIM_ASM_SGX_H
#include <linux/types.h>
/* No SGX host driver: KVM's SGX virtualization (vmx/sgx.c) is not built and
 * the CPUID leaves it would expose stay hidden. */
#define SGX_ATTR_PROVISIONKEY BIT_ULL(4)
#define SGX_ATTR_KSS          BIT_ULL(7)
#define SGX_ATTR_EINITTOKENKEY BIT_ULL(5)
#define SGX_ATTR_ASYNC_EXIT_NOTIFY BIT_ULL(10)
#define SGX_ATTR_RESERVED_MASK (BIT_ULL(3) | BIT_ULL(6) | GENMASK_ULL(9, 8) | GENMASK_ULL(63, 11))
#define SGX_MISC_EXINFO BIT(0)
#define SGX_MISC_RESERVED_MASK GENMASK_ULL(63, 1)
#define SGX_XFRM_RESERVED_MASK 0
#define SGX_ATTR_PRIV_MASK   (SGX_ATTR_PROVISIONKEY | SGX_ATTR_EINITTOKENKEY | SGX_ATTR_KSS)
#define SGX_ATTR_UNPRIV_MASK (~SGX_ATTR_PRIV_MASK & ~SGX_ATTR_RESERVED_MASK)
#endif
