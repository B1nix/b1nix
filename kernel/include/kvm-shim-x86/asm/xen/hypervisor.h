/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_XEN_HYPERVISOR_H
#define KVM_SHIM_ASM_XEN_HYPERVISOR_H
/* This kernel never runs as a Xen guest. */
#define xen_pv_domain()  0
#define xen_hvm_domain() 0
#endif
