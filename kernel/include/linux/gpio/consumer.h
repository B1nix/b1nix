/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_GPIO_CONSUMER_H
#define LKPI_LINUX_GPIO_CONSUMER_H
#include <linux/types.h>
/* GPIO lines, used by panel drivers on embedded boards. Neither M102 target has
 * any, so every lookup reports absence. */
struct gpio_desc;
struct device;
static inline struct gpio_desc *gpiod_get_optional(struct device *dev,
                                                   const char *con_id, int flags)
{ (void)dev; (void)con_id; (void)flags; return 0; }
static inline void gpiod_set_value(struct gpio_desc *d, int v)
{ (void)d; (void)v; }
static inline int gpiod_get_value(struct gpio_desc *d) { (void)d; return 0; }
#endif
