/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CLOCKSOURCE_H
#define LKPI_LINUX_CLOCKSOURCE_H
#include <linux/types.h>
enum vdso_clock_mode {
	VDSO_CLOCKMODE_NONE,
	VDSO_CLOCKMODE_TSC,
	VDSO_CLOCKMODE_PVCLOCK,
	VDSO_CLOCKMODE_HVCLOCK,
	VDSO_CLOCKMODE_MAX,
	VDSO_CLOCKMODE_TIMENS = INT_MAX
};
struct clocksource {
	u64 (*read)(struct clocksource *cs);
	u64 mask;
	u32 mult;
	u32 shift;
	const char *name;
	enum vdso_clock_mode vdso_clock_mode;
};
#endif
