/* SPDX-License-Identifier: MIT */
#ifndef LKPI_XEN_XEN_H
#define LKPI_XEN_XEN_H
/* Xen paravirtualisation. b1nix runs on bare metal and under KVM, never as a
 * Xen guest, so the checks imported code makes report "not Xen" and the
 * Xen-specific paths are never taken. */
#define xen_domain()      0
#define xen_pv_domain()   0
#define xen_initial_domain() 0
#endif
