/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_SPEC_CTRL_H
#define KVM_SHIM_ASM_SPEC_CTRL_H
#include <asm/nospec-branch.h>
/* Around a guest's run: the guest's SPEC_CTRL in, the host's back out. */
void x86_spec_ctrl_set_guest(u64 guest_virt_spec_ctrl);
void x86_spec_ctrl_restore_host(u64 guest_virt_spec_ctrl);
#endif
