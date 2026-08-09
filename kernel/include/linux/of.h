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
#endif
