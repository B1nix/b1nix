/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_FPU_API_H
#define LKPI_ASM_FPU_API_H
/*
 * Using SSE in kernel code.
 *
 * b1nix's kernel is compiled -mno-sse and has no FPU save area for kernel
 * threads, so there is no state to save and nothing may use vector registers
 * here. Any imported code that reaches a kernel_fpu_begin section is code that
 * must not be built — the movntdqa fast copies in i915 are exactly that, and
 * leaving these as no-ops would let such a path corrupt userspace FPU state
 * silently. They are declared, not defined, so it fails at link.
 */
void kernel_fpu_begin(void);
void kernel_fpu_end(void);
#endif
