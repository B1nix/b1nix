/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PID_H
#define LKPI_LINUX_PID_H
#include <linux/types.h>
/*
 * A refcounted handle on a process, so a driver can remember who opened a file
 * without pinning the whole task. b1nix's DRM paths are not yet entered from a
 * user task, so these carry the pid number and nothing holds a reference to a
 * task that could die — which is why get_pid is an identity and not a bug.
 */
struct pid { int nr; };
static inline struct pid *get_pid(struct pid *p) { return p; }
static inline void put_pid(struct pid *p) { (void)p; }
static inline pid_t pid_vnr(struct pid *p) { return p ? p->nr : 0; }
static inline struct pid *task_pid(void *task) { (void)task; return 0; }
static inline struct pid *task_tgid(void *task) { (void)task; return 0; }
static inline pid_t task_pid_vnr(void *task) { (void)task; return 0; }
static inline pid_t task_pid_nr(void *task) { (void)task; return 0; }

#define pid_nr(p) ((p) ? (p)->nr : 0)
#endif
