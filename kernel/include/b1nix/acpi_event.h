/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_ACPI_EVENT_H
#define B1NIX_ACPI_EVENT_H

#include <b1nix/types.h>

/*
 * ACPI events (M135): the SCI, the fixed events in PM1, the general-purpose
 * events and the Notify() calls their methods make.
 *
 * Nothing the platform raises reached the kernel before this: a power button,
 * a lid, an adapter plugged in, a battery changing state and a thermal trip
 * all arrive as an SCI, and the SCI had no handler. What this file does with
 * one is what Linux's ACPI core and its button, battery, AC, thermal and PCI
 * hotplug drivers do: a power or sleep button becomes a key on an input device
 * named as Linux names it, a lid a switch, a battery or an adapter a "change"
 * uevent on its power_supply, a card plugged into a hotplug slot a new PCI
 * function, and every event is counted under /sys/firmware/acpi/interrupts.
 */

/* Enable the SCI, route it, arm every GPE the firmware has a method for and
 * the fixed buttons it declares, and start the thread that runs the methods.
 * Needs the AML namespace, the input layer and the scheduler. No-op without an
 * FADT, and on a hardware-reduced platform, which has no SCI to route. */
void acpi_event_init(void);

/* Tell AML's Notify() where to go: called by the interpreter with the object's
 * path and the notification value, under the interpreter's lock, so it only
 * queues. */
void acpi_event_notify(const char *path, u64 value);

/* Power the machine off the way the firmware says to: \_PTS(5), then \_S5's
 * sleep type written to PM1a/PM1b control with SLP_EN. Returns only if the
 * platform has no \_S5 or ignored the write. */
void acpi_poweroff(void);

#endif /* B1NIX_ACPI_EVENT_H */
