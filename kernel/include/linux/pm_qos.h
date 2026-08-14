/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PM_QOS_H
#define LKPI_LINUX_PM_QOS_H
#include <linux/types.h>
#include <linux/notifier.h>

/*
 * Latency constraints on idle states.
 *
 * A driver publishes "do not let the CPU enter a state that takes longer than
 * N microseconds to leave", and the idle governor honours it. b1nix has no idle
 * governor and no deep C-states — the idle loop halts and an interrupt wakes it
 * — so there is no constraint to publish and nothing that would violate one.
 *
 * The requests are therefore recorded and never acted on. That is a real
 * difference and not a stub in disguise: on hardware with deep package states,
 * a driver that needs this to keep a display FIFO fed would underrun here, and
 * the fix would be an idle governor, not a change to this header.
 */
enum pm_qos_flags_status { PM_QOS_FLAGS_UNDEFINED = -1, PM_QOS_FLAGS_NONE, PM_QOS_FLAGS_SOME, PM_QOS_FLAGS_ALL };

#define PM_QOS_DEFAULT_VALUE ((s32)(-1))
#define PM_QOS_RESUME_LATENCY_NO_CONSTRAINT ((s32)0x7fffffff)
#define PM_QOS_CPU_LATENCY_DEFAULT_VALUE ((s32)0x7fffffff)

struct pm_qos_request {
	s32 value;
	bool active;
};

static inline void cpu_latency_qos_add_request(struct pm_qos_request *req, s32 value)
{ if (req) { req->value = value; req->active = true; } }
static inline void cpu_latency_qos_update_request(struct pm_qos_request *req, s32 value)
{ if (req) req->value = value; }
static inline void cpu_latency_qos_remove_request(struct pm_qos_request *req)
{ if (req) { req->active = false; req->value = PM_QOS_DEFAULT_VALUE; } }
static inline bool cpu_latency_qos_request_active(struct pm_qos_request *req)
{ return req && req->active; }
#endif
