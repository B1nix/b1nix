/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_TRAPS_H
#define KVM_SHIM_ASM_TRAPS_H
/* Linux's trap handler declarations. SVM includes it and uses none of them:
 * its intercepted exceptions are reflected into the guest, never run as the
 * host's own handlers. */
#endif
