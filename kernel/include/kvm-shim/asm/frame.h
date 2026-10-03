/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_FRAME_H
#define KVM_SHIM_ASM_FRAME_H
/* A frame for assembly functions, so a backtrace walks through them: b1nix
 * keeps frame pointers, as upstream's CONFIG_FRAME_POINTER does. */
#ifdef __ASSEMBLY__
.macro FRAME_BEGIN
	push %rbp
	mov %rsp, %rbp
.endm
.macro FRAME_END
	pop %rbp
.endm
#endif
#endif
