/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_LINUX_PERF_EVENT_H
#define KVM_SHIM_LINUX_PERF_EVENT_H
/*
 * The perf interface KVM's vPMU is written against (M131). b1nix's own perf
 * (M126) is not exposed to imported code, so a kernel counter cannot be
 * created and perf_get_x86_pmu_capability reports no PMU: KVM then keeps the
 * vPMU off, exactly as it does on a Linux host without a PMU driver.
 */
#include_next <linux/perf_event.h>
#include <uapi/linux/perf_event.h>
#include <linux/err.h>

enum perf_event_state {
	PERF_EVENT_STATE_DEAD = -4,
	PERF_EVENT_STATE_EXIT = -3,
	PERF_EVENT_STATE_ERROR = -2,
	PERF_EVENT_STATE_OFF = -1,
	PERF_EVENT_STATE_INACTIVE = 0,
	PERF_EVENT_STATE_ACTIVE = 1,
};

struct perf_sample_data;
struct pt_regs;
typedef void (*perf_overflow_handler_t)(struct perf_event *, struct perf_sample_data *,
					struct pt_regs *);

struct perf_event {
	struct perf_event_attr attr;
	struct {
		int idx;
		u64 config;
	} hw;
	enum perf_event_state state;
	int oncpu;
	void *overflow_handler_context;
};

static inline struct perf_event *
perf_event_create_kernel_counter(struct perf_event_attr *attr, int cpu,
				 struct task_struct *task,
				 perf_overflow_handler_t callback, void *context)
{
	(void)attr; (void)cpu; (void)task; (void)callback; (void)context;
	return ERR_PTR(-ENODEV);
}
static inline int perf_event_release_kernel(struct perf_event *e) { (void)e; return 0; }
static inline u64 perf_event_read_value(struct perf_event *e, u64 *enabled, u64 *running)
{
	(void)e;
	*enabled = *running = 0;
	return 0;
}
static inline void perf_event_enable(struct perf_event *e) { (void)e; }
static inline u64 perf_event_pause(struct perf_event *e, bool reset) { (void)e; (void)reset; return 0; }
static inline int perf_event_period(struct perf_event *e, u64 value) { (void)e; (void)value; return -ENODEV; }
static inline bool is_sampling_event(struct perf_event *e) { return e->attr.sample_period != 0; }

struct perf_guest_info_callbacks {
	unsigned int (*state)(void);
	unsigned long (*get_ip)(void);
	unsigned int (*handle_intel_pt_intr)(void);
};
static inline void perf_register_guest_info_callbacks(struct perf_guest_info_callbacks *cbs) { (void)cbs; }
static inline void perf_unregister_guest_info_callbacks(struct perf_guest_info_callbacks *cbs) { (void)cbs; }
#define PERF_GUEST_ACTIVE 0x01
#define PERF_GUEST_USER   0x02
struct perf_guest_switch_msr {
	unsigned int msr;
	u64 host;
	u64 guest;
};
#endif
