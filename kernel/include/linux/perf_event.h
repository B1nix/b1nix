/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PERF_EVENT_H
#define LKPI_LINUX_PERF_EVENT_H
#include <linux/types.h>
/*
 * The perf subsystem, which i915 registers a PMU against so userspace can read
 * engine busyness and frequency. b1nix has no perf, so nothing registers and no
 * counters are exported — the driver's own state is still there, just not
 * through this interface.
 */
struct perf_event;
struct pmu { int dummy; };
#endif
