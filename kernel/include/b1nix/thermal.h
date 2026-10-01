/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_THERMAL_H
#define B1NIX_THERMAL_H

#include <b1nix/types.h>

/*
 * Thermal management (M135): the ACPI thermal zones M134 could only read,
 * acted on.
 *
 * Each zone's trip points come from the firmware — `_CRT` (critical: power
 * off), `_HOT` (hot: tell userspace), `_PSV` (passive: slow the processor
 * down) and `_AC0`..`_AC9` (active: turn fans on) — and so do the cooling
 * devices: the processor's P-states for the passive trip, and the fans that
 * `_ALx` names, switched through the power resources in their `_PR0`. A
 * thread re-reads a zone on `Notify(zone, 0x80)`, every `_TZP`, and every
 * `_TSP` while it is cooling passively, whose step is ACPI's own formula
 * (`_TC1`, `_TC2`).
 *
 * Temperatures here are millidegrees Celsius, as /sys/class/thermal has them.
 */

#define THERMAL_MAX_TRIPS 14
#define THERMAL_MAX_CDEV 8

/* Read the zones' trips, find their cooling devices and start the thread.
 * After acpi_power_init() and cpufreq_init(), with the scheduler running. */
void thermal_init(void);

/* Notify(zone, value) on zone `idx`: 0x80 temperature, 0x81 trip points. */
void thermal_notify(int idx, u64 value);

/* The temperature the zone acts on: emul_temp when one is set, else _TMP.
 * Returns 0, or -1 when the firmware would not answer. */
int thermal_zone_temp(int zone, int *mc);
/* thermal_zoneN/emul_temp: 0 goes back to the sensor. */
int thermal_zone_set_emul(int zone, int mc);
int thermal_zone_emul(int zone);

int thermal_trip_count(int zone);
/* "critical", "hot", "passive" or "active", and the temperature. */
int thermal_trip(int zone, int trip, const char **type, int *mc);

int thermal_cdev_count(void);
const char *thermal_cdev_type(int cdev);   /* "Processor" or "Fan" */
int thermal_cdev_max_state(int cdev);
int thermal_cdev_cur_state(int cdev);      /* what the device says it is in */
int thermal_cdev_set_state(int cdev, int state);

/* The zone's bindings: the k-th cooling device it drives and the trip that
 * drives it, for thermal_zoneN/cdevK and cdevK_trip_point. */
int thermal_zone_binding(int zone, int k, int *cdev, int *trip);

#endif /* B1NIX_THERMAL_H */
