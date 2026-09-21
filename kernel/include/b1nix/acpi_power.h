/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_ACPI_POWER_H
#define B1NIX_ACPI_POWER_H

#include <b1nix/types.h>

/*
 * The consumers of the AML interpreter (M134): the ACPI battery, the AC
 * adapter and thermal zones.
 *
 * Every number here comes from evaluating a method in the firmware's own
 * bytecode — `_BST` for the charge, `_BIF` for the capacity it is a fraction
 * of, `_PSR` for whether the mains are connected, `_TMP` for a temperature.
 * Nothing is published for a device the firmware does not declare: on a
 * machine with no battery, /sys/class/power_supply is EMPTY, which is the
 * true answer and the only safe one.
 */

#define ACPI_PS_MAX_BATTERY 2
#define ACPI_PS_MAX_AC      1
#define ACPI_PS_MAX_THERMAL 4

/* Battery attributes, in the order /sys/class/power_supply/BAT0 wants them. */
#define ACPI_BAT_TYPE        0
#define ACPI_BAT_PRESENT     1
#define ACPI_BAT_STATUS      2
#define ACPI_BAT_CAPACITY    3
#define ACPI_BAT_NOW         4   /* energy_now or charge_now */
#define ACPI_BAT_FULL        5   /* energy_full or charge_full */

/* Thermal zone attributes. */
#define ACPI_TZ_TYPE         0
#define ACPI_TZ_TEMP         1

/* Find the devices, if the firmware has any. Call after aml_init(). */
void acpi_power_init(void);

int acpi_power_battery_count(void);
int acpi_power_ac_count(void);
int acpi_power_thermal_count(void);

/* Is the battery reporting in energy units (mWh, so energy_*) rather than
 * charge units (mAh, so charge_*)? Linux names the files after the unit the
 * firmware chose, and a reader that divides one by the other has to know. */
int acpi_power_battery_in_energy_units(int idx);

/* Render one attribute as sysfs content, newline included. Returns the number
 * of bytes written, or a negative value when the firmware would not answer —
 * in which case the file must not claim a number. */
int acpi_power_battery_attr(int idx, int which, char *buf, usize cap);
int acpi_power_ac_attr(int idx, char *buf, usize cap);   /* "online" */
int acpi_power_thermal_attr(int idx, int which, char *buf, usize cap);

/* The namespace path of a discovered device, for /proc and the boot log. */
const char *acpi_power_battery_path(int idx);
const char *acpi_power_ac_path(int idx);
const char *acpi_power_thermal_path(int idx);

#endif /* B1NIX_ACPI_POWER_H */
