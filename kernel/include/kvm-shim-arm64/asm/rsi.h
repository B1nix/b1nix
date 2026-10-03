/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_RSI_H
#define KVM_SHIM_ARM64_ASM_RSI_H
/* The Realm Services Interface: b1nix never runs as an Arm CCA realm guest,
 * so no memory is realm-protected and the NS-shared attribute is never set. */
#include <linux/types.h>
static inline bool is_realm_world(void) { return false; }
#endif
