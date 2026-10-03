/* SPDX-License-Identifier: GPL-2.0-only */
/* Included ahead of every KVM source (M131): what Linux's own header chains
 * would have brought in by the time KVM's headers use it -- the shim's
 * headers are flatter and do not drag the whole kernel along, so these are
 * named once here instead of in each header that happens to need one. */
#ifndef KVM_SHIM_PRELUDE_H
#define KVM_SHIM_PRELUDE_H
#include <linux/errno.h>
#include <linux/percpu.h>
#include <linux/hrtimer.h>
#include <linux/cpumask.h>
#include <asm/pgtable_types.h>
#include <asm/pgtable.h>
#include <linux/profile.h>
#include <linux/file.h>
#include <linux/sched/smt.h>
#include <asm/nospec-branch.h>
#include <asm/tlbflush.h>
#include <asm/posted_intr.h>
#include <asm/desc.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>
#include <asm/fpu/types.h>
/* Named by asm/perf_event.h's no-perf branch, which only defines it when
 * CONFIG_PERF_EVENTS is set: KVM builds without the host PMU driver, and
 * perf reports no capabilities, so the vPMU stays off. */
struct x86_pmu_lbr {
	unsigned int nr;
	unsigned int from;
	unsigned int to;
	unsigned int info;
	bool has_callstack;
};
#include <asm/perf_event.h>
#endif
