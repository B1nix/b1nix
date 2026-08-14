/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PINCTRL_CONSUMER_H
#define LKPI_LINUX_PINCTRL_CONSUMER_H
#include <linux/err.h>
/* Pin multiplexing, for the SoC panel wiring described in <linux/pwm.h>. No
 * pin controller exists here, so a lookup fails and the driver takes its
 * native path. */
struct pinctrl;
struct pinctrl_state;
static inline struct pinctrl *devm_pinctrl_get(struct device *dev)
{ (void)dev; return ERR_PTR(-ENODEV); }
static inline struct pinctrl_state *pinctrl_lookup_state(struct pinctrl *p, const char *name)
{ (void)p; (void)name; return ERR_PTR(-ENODEV); }
static inline int pinctrl_select_state(struct pinctrl *p, struct pinctrl_state *s)
{ (void)p; (void)s; return -ENODEV; }

/* Get a pin controller and select a named state in one step. No controller
 * exists here — see <linux/pinctrl/machine.h> — so the lookup fails and the
 * caller carries on without touching pin muxing. */
static inline struct pinctrl *devm_pinctrl_get_select(struct device *dev,
                                                      const char *name)
{ (void)dev; (void)name; return ERR_PTR(-ENODEV); }

#endif
