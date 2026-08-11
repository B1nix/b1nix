/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_OF_H
#define LKPI_LINUX_OF_H
#include <linux/types.h>
/* Device tree. x86 has none — firmware describes hardware through ACPI — so
 * every lookup reports absence. This is not a stub of something missing; there
 * genuinely is no device tree on the machines M102 targets. */
struct device_node;
static inline struct device_node *of_find_node_by_name(struct device_node *f,
                                                       const char *n)
{ (void)f; (void)n; return 0; }
static inline int of_property_read_u32(const struct device_node *n,
                                       const char *p, u32 *v)
{ (void)n; (void)p; (void)v; return -EINVAL; }
static inline bool of_property_read_bool(const struct device_node *n,
                                         const char *p)
{ (void)n; (void)p; return false; }

/*
 * Device-tree lookups.
 *
 * b1nix boots from Multiboot2 on x86_64 and has no device tree: every device is
 * found by PCI enumeration or by ACPI. So every lookup here reports absence,
 * which is what upstream does on a kernel built without CONFIG_OF, and every
 * caller falls back to the path it takes on a PC.
 */
struct device_node;
struct fwnode_handle;
struct device_driver;
static inline void of_node_put(struct device_node *node) { (void)node; }
static inline struct device_node *of_node_get(struct device_node *node)
{ return node; }
static inline struct device_node *of_parse_phandle(const struct device_node *np,
                                                   const char *name, int index)
{ (void)np; (void)name; (void)index; return NULL; }
static inline struct fwnode_handle *of_fwnode_handle(struct device_node *node)
{ (void)node; return NULL; }
static inline int of_driver_match_device(struct device *dev,
                                         const struct device_driver *drv)
{ (void)dev; (void)drv; return 0; }
static inline int of_device_uevent_modalias(const struct device *dev,
                                            struct kobj_uevent_env *env)
{ (void)dev; (void)env; return -ENODEV; }


static inline bool of_property_present(const struct device_node *np,
                                       const char *name)
{ (void)np; (void)name; return false; }
/* Walk a node's enabled children. There are no nodes — see above — so the walk
 * visits nothing and the body never runs. */
#define for_each_available_child_of_node(parent, child) \
	for ((child) = NULL; (child); )


/* Callers reach the DSI host list's lock through this header's chain upstream. */
#include <linux/mutex.h>

#endif
