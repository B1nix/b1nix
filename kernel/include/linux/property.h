/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PROPERTY_H
#define LKPI_LINUX_PROPERTY_H
#include <linux/device.h>
#include <linux/of.h>
#include <linux/err.h>
/* The firmware-agnostic property lookup that sits over device tree and ACPI.
 * b1nix has neither source wired up, so every read reports absence. */
/* A firmware node. b1nix resolves nothing through one, but imported code
 * compares the `secondary` link, so the field exists and is always NULL. */
struct fwnode_handle {
	struct fwnode_handle *secondary;
};
static inline void fwnode_handle_put(struct fwnode_handle *f) { (void)f; }
static inline struct fwnode_handle *dev_fwnode(struct device *d)
{ (void)d; return 0; }
static inline int device_property_read_u32(struct device *dev, const char *p,
                                           u32 *v)
{ (void)dev; (void)p; (void)v; return -EINVAL; }
static inline bool device_property_read_bool(struct device *dev, const char *p)
{ (void)dev; (void)p; return false; }

/* No firmware nodes are resolved here (see above): nothing is available,
 * referenced or present. */
static inline bool fwnode_device_is_available(const struct fwnode_handle *f)
{ (void)f; return false; }
static inline struct fwnode_handle *
fwnode_find_reference(const struct fwnode_handle *f, const char *name, unsigned int index)
{ (void)f; (void)name; (void)index; return ERR_PTR(-ENOENT); }
static inline bool device_property_present(const struct device *dev, const char *p)
{ (void)dev; (void)p; return false; }

#endif
