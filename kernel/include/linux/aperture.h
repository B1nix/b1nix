/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_APERTURE_H
#define LKPI_LINUX_APERTURE_H
#include <linux/types.h>
/* Kicking the firmware framebuffer out of the way before a real driver takes
 * the hardware. b1nix's boot framebuffer is owned by its own code, and handing
 * it over is part of bringing up the first vendor driver, not of the core. */
struct pci_dev;
static inline int aperture_remove_conflicting_devices(u64 base, u64 size,
                                                      const char *name)
{ (void)base; (void)size; (void)name; return 0; }
struct drm_device;
static inline int devm_aperture_acquire_for_platform_device(void *pdev, u64 base,
                                                            u64 size)
{ (void)pdev; (void)base; (void)size; return 0; }

static inline int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev,
                                                          const char *name)
{ (void)pdev; (void)name; return 0; }
#endif
