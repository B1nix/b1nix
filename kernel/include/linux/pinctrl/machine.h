/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PINCTRL_MACHINE_H
#define LKPI_LINUX_PINCTRL_MACHINE_H
#include <linux/pinctrl/consumer.h>
/* Board-level pin mapping tables. Same as <linux/gpio/machine.h>: no controller
 * exists here, so a registered table is never matched. */
struct pinctrl_map { int dummy; };
static inline int pinctrl_register_mappings(const struct pinctrl_map *map, unsigned num)
{ (void)map; (void)num; return 0; }
static inline void pinctrl_unregister_mappings(const struct pinctrl_map *map)
{ (void)map; }

/* A mapping entry that selects a mux group. Never matched here — see above. */
#define PIN_MAP_MUX_GROUP(_dev, _state, _ctrl, _grp, _func) { 0 }

#endif
