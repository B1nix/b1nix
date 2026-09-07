/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_COMPONENT_H
#define LKPI_LINUX_COMPONENT_H
#include <linux/device.h>
/* The component framework binds a master device to sub-devices that probe
 * independently — an SoC display controller plus its encoders. Neither M102
 * target is built that way (both are single PCI functions), so these report
 * absence rather than implement a binding nothing uses. */
struct component_master_ops;
/* What a sub-device registers with. Nothing here is composed of several
 * devices, so nothing registers. */
struct component_ops {
	int (*bind)(struct device *, struct device *, void *);
	void (*unbind)(struct device *, struct device *, void *);
};
struct component_match;
/* Register a sub-device with the component framework. Nothing here is composed
 * of several devices — see below — so this reports absence. */
struct component_ops;
/*
 * The refusal is deliberate, and it was measured.
 *
 * i915 prints "failed to add audio component (-19)" for it, which reads like
 * something worth silencing — so it was tried: report success, let the
 * component sit in a list nothing will ever bind, exactly as upstream does on
 * a machine whose audio driver never appears. On the passed-through UHD 630
 * that wedged the GPU. Engine enumeration finished and the first request
 * submitted to rcs0 never came back, where the same image with the refusal in
 * place runs all four engines in ten milliseconds each.
 *
 * So this stays -ENODEV, the message stays with it, and it is accurate: there
 * is no component framework here to add anything to.
 */
static inline int component_add(struct device *dev,
                                const struct component_ops *ops)
{ (void)dev; (void)ops; return -ENODEV; }
static inline void component_del(struct device *dev,
                                 const struct component_ops *ops)
{ (void)dev; (void)ops; }

static inline int component_master_add_with_match(struct device *d,
                                                  const struct component_master_ops *o,
                                                  struct component_match *m)
{ (void)d; (void)o; (void)m; return -ENODEV; }
static inline void component_master_del(struct device *d,
                                        const struct component_master_ops *o)
{ (void)d; (void)o; }

/* Binding a driver into an aggregate device — i915 uses it to attach the audio
 * codec on the same package. b1nix has no component framework, so the
 * registration is recorded as failed rather than pretended: a driver told its
 * component was added would then wait for a bind that never comes. */
struct component_ops;
static inline int component_add_typed(struct device *dev,
                                      const struct component_ops *ops, int subcomponent)
{ (void)dev; (void)ops; (void)subcomponent; return -ENODEV; }

#endif
