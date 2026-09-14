/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PERCPU_H
#define LKPI_LINUX_PERCPU_H

#include <linux/types.h>
#include <linux/cpumask.h>
#include <linux/slab.h>

/*
 * Per-CPU variables, as a single shared instance.
 *
 * ext4 keeps a per-CPU counter of allocation attempts and btrfs a per-CPU
 * preference for which block group to allocate from. Both are HINTS: the value
 * steers a decision and is never the decision itself, and both filesystems
 * re-read the authoritative state under a lock afterwards.
 *
 * So one instance shared by every CPU is correct here, and the cost is
 * contention on a cache line rather than a wrong answer. That is a deliberate
 * simplification and it has a boundary: a per-CPU variable whose value must be
 * exactly right — a counter that is summed rather than sampled — does not
 * belong here, and <linux/percpu_counter.h> is where that case is handled.
 */

#define DEFINE_PER_CPU(type, name) type name
#define EXPORT_PER_CPU_SYMBOL(var)     extern int lkpi_export_marker_unused
#define EXPORT_PER_CPU_SYMBOL_GPL(var) extern int lkpi_export_marker_unused
#define DECLARE_PER_CPU(type, name) extern type name
#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name) type name
#define __percpu

/* Every CPU sees the same object, so the id is ignored — deliberately, not by
 * omission: the alternative is an array nothing indexes correctly on a kernel
 * with no per-CPU sections. */
#define per_cpu(var, cpu)      (*({ (void)(cpu); &(var); }))
#define per_cpu_ptr(ptr, cpu)  ({ (void)(cpu); (ptr); })
#define this_cpu_ptr(ptr)      (ptr)
#define get_cpu_ptr(ptr)       (ptr)
#define put_cpu_ptr(ptr)       do { (void)(ptr); } while (0)
#define get_cpu_var(var)       (var)
#define put_cpu_var(var)       do { } while (0)
#define this_cpu_read(var)     (var)
#define this_cpu_write(var, v) ((var) = (v))
#define this_cpu_inc(var)      ((var)++)
#define this_cpu_dec(var)      ((var)--)
#define this_cpu_add(var, n)   ((var) += (n))

static inline void *__alloc_percpu(size_t size, size_t align)
{
	(void)align;
	return kzalloc(size, GFP_KERNEL);
}
#define alloc_percpu(type)        ((type *)__alloc_percpu(sizeof(type), __alignof__(type)))
#define alloc_percpu_gfp(type, g) ({ (void)(g); alloc_percpu(type); })
static inline void free_percpu(void *p) { kfree(p); }

/* The unchecked form of this_cpu_ptr: same object, no debug assertion about
 * preemption, because there is no per-CPU data to be wrong about. */
#define raw_cpu_ptr(ptr) (ptr)

#endif
