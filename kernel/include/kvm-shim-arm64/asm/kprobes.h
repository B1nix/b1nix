/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_KPROBES_H
#define KVM_SHIM_ARM64_ASM_KPROBES_H
/* b1nix has no kprobes, so the blacklist annotation on KVM's world-switch
 * code has nothing to keep a probe out of. */
#define NOKPROBE_SYMBOL(fname)
#define __kprobes
#endif
