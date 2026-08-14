/* SPDX-License-Identifier: GPL-2.0-only */
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

/* A GPIO line looked up by index. No provider exists here — see
 * <linux/gpio/machine.h> — so the lookup fails and the driver takes its native
 * path rather than driving a line that is not wired to anything. */
struct gpio_desc;
static inline struct gpio_desc *devm_gpiod_get_index(struct device *dev,
                                                     const char *con_id,
                                                     unsigned int idx, int flags)
{ (void)dev; (void)con_id; (void)idx; (void)flags; return ERR_PTR(-ENOENT); }


/* Flags for a line request: direction, and the level to drive immediately. No
 * provider exists here (see above), so a request never succeeds and the level
 * is never driven. */
enum gpiod_flags {
	GPIOD_ASIS = 0,
	GPIOD_IN = 1,
	GPIOD_OUT_LOW = 2,
	GPIOD_OUT_HIGH = 3,
};

/* The sleepable setter, for a line behind a bus that cannot be driven from
 * atomic context. Same absence as gpiod_set_value(). */
static inline void gpiod_set_value_cansleep(struct gpio_desc *desc, int value)
{ (void)desc; (void)value; }


/* The non-optional lookup: same absence as gpiod_get_optional(), reported as an
 * error rather than as NULL. */
static inline struct gpio_desc *gpiod_get(struct device *dev, const char *con_id,
                                          enum gpiod_flags flags)
{ (void)dev; (void)con_id; (void)flags; return ERR_PTR(-ENOENT); }
static inline void gpiod_put(struct gpio_desc *desc) { (void)desc; }

#endif
