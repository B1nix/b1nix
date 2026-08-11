/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_VGAARB_H
#define LKPI_LINUX_VGAARB_H
#include <linux/types.h>
/* VGA arbitration decides which of several cards owns the legacy VGA I/O
 * ranges. b1nix drives one GPU at a time, so there is nothing to arbitrate and
 * these are no-ops — a statement about the configuration, not a stub of a
 * mechanism we need. */
struct pci_dev;
#define VGA_RSRC_LEGACY_IO 0x01
static inline void vga_set_legacy_decoding(struct pci_dev *d, unsigned int r)
{ (void)d; (void)r; }
static inline int vga_client_register(struct pci_dev *d, void *set_decode)
{ (void)d; (void)set_decode; return -ENODEV; }

#define VGA_RSRC_NORMAL_IO  0x02
#define VGA_RSRC_NORMAL_MEM 0x04
#define VGA_RSRC_LEGACY_MEM 0x08

/* Take and release the legacy VGA resources. One GPU is driven here — see the
 * note above — so ownership is never contested and these grant immediately. */
static inline int vga_get_uninterruptible(struct pci_dev *pdev, unsigned int rsrc)
{ (void)pdev; (void)rsrc; return 0; }
static inline int vga_get_interruptible(struct pci_dev *pdev, unsigned int rsrc)
{ (void)pdev; (void)rsrc; return 0; }
static inline void vga_put(struct pci_dev *pdev, unsigned int rsrc)
{ (void)pdev; (void)rsrc; }
static inline void vga_client_unregister(struct pci_dev *pdev) { (void)pdev; }

#endif
