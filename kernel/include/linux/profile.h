/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PROFILE_H
#define LKPI_LINUX_PROFILE_H
#define KVM_PROFILING 4
#define prof_on 0
/* The kernel profiler (profile=) does not exist here. */
static inline void profile_hit(int type, void *ip) { (void)type; (void)ip; }
#endif
