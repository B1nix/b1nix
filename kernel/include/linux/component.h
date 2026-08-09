/* SPDX-License-Identifier: MIT */
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
#endif
