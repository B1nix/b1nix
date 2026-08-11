/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_GPIO_MACHINE_H
#define LKPI_LINUX_GPIO_MACHINE_H
/* Board-level GPIO lookup tables, for the SoC panel wiring described in
 * <linux/pwm.h>. No provider exists here, so a table registered against one is
 * never matched. */
struct gpiod_lookup { const char *key; u16 chip_hwnum; const char *con_id;
                      unsigned int idx; unsigned long flags; };
struct gpiod_lookup_table {
	struct list_head list;
	const char *dev_id;
	struct gpiod_lookup table[];
};
static inline void gpiod_add_lookup_table(struct gpiod_lookup_table *t) { (void)t; }
static inline void gpiod_remove_lookup_table(struct gpiod_lookup_table *t) { (void)t; }

#define GPIO_ACTIVE_HIGH 0
#define GPIO_ACTIVE_LOW  1
/* An entry in a board's lookup table. The table is never matched here — see
 * above — so the entry exists to let a driver's table compile. */
#define GPIO_LOOKUP(_chip_label, _chip_hwnum, _con_id, _flags) { 0 }

#endif
