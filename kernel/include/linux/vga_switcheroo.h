/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_VGA_SWITCHEROO_H
#define LKPI_LINUX_VGA_SWITCHEROO_H
#include <linux/types.h>
/* Muxing a laptop's display between integrated and discrete GPUs. Handling it
 * needs ACPI methods b1nix cannot evaluate, so every entry point reports
 * absence — on the Pavilion that means the panel stays on whichever GPU the
 * firmware left it. */
struct pci_dev;
enum vga_switcheroo_state { VGA_SWITCHEROO_OFF, VGA_SWITCHEROO_ON };
static inline int vga_switcheroo_register_client(struct pci_dev *d, const void *o, bool m)
{ (void)d; (void)o; (void)m; return -ENODEV; }
static inline void vga_switcheroo_unregister_client(struct pci_dev *d) { (void)d; }
static inline void vga_switcheroo_client_fb_set(struct pci_dev *d, void *i)
{ (void)d; (void)i; }
static inline int vga_switcheroo_process_delayed_switch(void) { return 0; }
/* Serialising DDC against the other GPU's driver. With no mux there is nothing
 * to serialise against. */
static inline int vga_switcheroo_lock_ddc(struct pci_dev *d)
{ (void)d; return -ENODEV; }
static inline int vga_switcheroo_unlock_ddc(struct pci_dev *d)
{ (void)d; return -ENODEV; }
#endif
