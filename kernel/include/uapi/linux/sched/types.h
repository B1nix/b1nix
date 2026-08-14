/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_UAPI_LINUX_SCHED_TYPES_H
#define LKPI_UAPI_LINUX_SCHED_TYPES_H
#include <linux/types.h>
/* The scheduling-attribute struct sched_setattr(2) takes. The DRM scheduler
 * names it when it asks for a real-time priority for its submission thread;
 * b1nix has priorities but not this interface, so the struct exists and the
 * request is declined by whoever would honour it. */
struct sched_attr {
	__u32 size;
	__u32 sched_policy;
	__u64 sched_flags;
	__s32 sched_nice;
	__u32 sched_priority;
	__u64 sched_runtime;
	__u64 sched_deadline;
	__u64 sched_period;
};
#endif
