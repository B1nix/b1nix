/* SPDX-License-Identifier: MIT */
#ifndef LKPI_ASM_SMP_H
#define LKPI_ASM_SMP_H
#include <linux/smp.h>

/*
 * Cache maintenance and the FPU gate, reachable from the header imported x86
 * code includes for this purpose. Linux spreads these across
 * <asm/special_insns.h> and <asm/fpu/api.h>; drm_cache.c reaches them through
 * this one, so this is where they have to be visible.
 */
#include <asm/cacheflush.h>
#include <asm/fpu/api.h>

#endif
