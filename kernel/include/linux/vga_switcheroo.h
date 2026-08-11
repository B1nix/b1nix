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

/* Whether probing should be deferred because the other GPU in a switchable
 * pair has not registered yet. b1nix has no switcheroo, so nothing defers —
 * and answering "defer" when nothing will ever complete the handshake would
 * stall the probe forever. */
static inline int vga_switcheroo_client_probe_defer(struct pci_dev *pdev)
{ (void)pdev; return 0; }


/* The client hooks a switchable GPU publishes. No switcheroo here, so nothing
 * registers — see vga_switcheroo_client_probe_defer, which says so. */
struct vga_switcheroo_client_ops {
	void (*set_gpu_state)(struct pci_dev *dev, enum vga_switcheroo_state state);
	void (*reprobe)(struct pci_dev *dev);
	bool (*can_switch)(struct pci_dev *dev);
	void (*gpu_bound)(struct pci_dev *dev, int id);
};


/* Whether a switcheroo handler is present. None is, so the answer is no and a
 * driver keeps control of its own panel power. The register/unregister pair is
 * already declared above. */
static inline bool vga_switcheroo_handler_flags(void) { return false; }


/* What a client reports it can do. Nothing switches here, so the value only
 * has to exist for the driver's own table. */
#define VGA_SWITCHEROO_CAN_SWITCH_DDC (1 << 0)
#define VGA_SWITCHEROO_NEEDS_EDP_CONFIG (1 << 1)
#define VGA_SWITCHEROO_ON  1
#define VGA_SWITCHEROO_OFF 0

#endif
