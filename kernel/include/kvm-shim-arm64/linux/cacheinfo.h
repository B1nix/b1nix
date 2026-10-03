/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_CACHEINFO_H
#define KVM_SHIM_ARM64_LINUX_CACHEINFO_H
/* The cache-level kinds, as CLIDR_EL1 encodes them. KVM builds a guest's
 * CLIDR from these; the host's cache topology tables themselves are not used. */
enum cache_type {
	CACHE_TYPE_NOCACHE = 0,
	CACHE_TYPE_INST = 1,
	CACHE_TYPE_DATA = 2,
	CACHE_TYPE_SEPARATE = CACHE_TYPE_INST | CACHE_TYPE_DATA,
	CACHE_TYPE_UNIFIED = 4,
};
#endif
