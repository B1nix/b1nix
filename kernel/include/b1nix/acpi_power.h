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
#define ACPI_BAT_VOLTAGE     6   /* voltage_now, in microvolts */
#define ACPI_BAT_RATE        7   /* power_now or current_now */
/* M135: the rest of what Linux's ACPI battery publishes, from _BIX where the
 * firmware has it and _BIF where it does not. */
#define ACPI_BAT_FULL_DESIGN 8   /* energy_full_design or charge_full_design */
#define ACPI_BAT_VOLTAGE_MIN 9   /* voltage_min_design */
#define ACPI_BAT_CYCLES      10  /* cycle_count (_BIX only) */
#define ACPI_BAT_TECHNOLOGY  11
#define ACPI_BAT_MODEL       12
#define ACPI_BAT_SERIAL      13
#define ACPI_BAT_MANUFACTURER 14
#define ACPI_BAT_CAPACITY_LEVEL 15
#define ACPI_BAT_ALARM       16  /* the _BTP trip, in µWh or µAh */
#define ACPI_BAT_CHARGE_BEHAVIOUR 17 /* _BMD/_BMC: auto, inhibit-charge, force-discharge */

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
/* Does the battery have a _BIX (cycle_count), and a _BTP (alarm)? */
int acpi_power_battery_has(int idx, const char *method);
/* alarm: set the capacity (µWh or µAh) at which the firmware notifies. */
int acpi_power_battery_set_alarm(int idx, u64 micro);
/* charge_behaviour through ACPI battery maintenance (_BMD reports what the
 * firmware can do and is doing, _BMC asks for it): "auto", "inhibit-charge" or
 * "force-discharge". Only offered when _BMD says _BMC can do more than auto. */
int acpi_power_battery_has_charge_control(int idx);
int acpi_power_battery_set_charge_behaviour(int idx, const char *name);
int acpi_power_thermal_attr(int idx, int which, char *buf, usize cap);

/* The namespace path of a discovered device, for /proc and the boot log. */
const char *acpi_power_battery_path(int idx);
const char *acpi_power_ac_path(int idx);
const char *acpi_power_thermal_path(int idx);

#endif /* B1NIX_ACPI_POWER_H */
