/* SPDX-License-Identifier: MIT */
#ifndef LKPI_ACPI_BUTTON_H
#define LKPI_ACPI_BUTTON_H
#include <linux/notifier.h>
/* The lid switch, as an ACPI event source. Without an ACPI interpreter there
 * are no events, so a registered notifier is never called and the lid reads as
 * open — the state a machine with no lid is in. */
#define ACPI_BUTTON_LID_INIT_OPEN 1
static inline int acpi_lid_open(void) { return 1; }
static inline int acpi_lid_notifier_register(struct notifier_block *nb)
{ (void)nb; return 0; }
static inline int acpi_lid_notifier_unregister(struct notifier_block *nb)
{ (void)nb; return 0; }
#endif
