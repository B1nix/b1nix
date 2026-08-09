/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SMP_H
#define LKPI_LINUX_SMP_H
#include <lkpi/env.h>
#include <linux/types.h>
static inline int smp_processor_id(void) { return (int)lkpi_cpu_id(); }
#define raw_smp_processor_id() smp_processor_id()
#define num_online_cpus()      lkpi_cpu_count()
#define smp_mb()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_load_acquire(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#endif
