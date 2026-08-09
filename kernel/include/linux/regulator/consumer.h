/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_REGULATOR_CONSUMER_H
#define LKPI_LINUX_REGULATOR_CONSUMER_H
#include <linux/device.h>
/* Board power rails. A PCI card powers itself; these exist for panel drivers on
 * embedded boards and report absence. */
struct regulator;
static inline struct regulator *devm_regulator_get_optional(struct device *dev,
                                                            const char *id)
{ (void)dev; (void)id; return 0; }
static inline int regulator_enable(struct regulator *r) { (void)r; return -ENODEV; }
static inline int regulator_disable(struct regulator *r) { (void)r; return -ENODEV; }
#endif
