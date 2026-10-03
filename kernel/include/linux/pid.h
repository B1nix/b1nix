/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PID_H
#define LKPI_LINUX_PID_H
#include <linux/types.h>
#include <lkpi/env.h>
/*
 * A refcounted handle on a process, so a driver can remember who opened a file
 * without pinning the whole task. The handle is part of the lkpi task (struct
 * pid in <lkpi/env.h>), which lives as long as the kernel, so nothing has to
 * hold a reference -- which is why get_pid is an identity and not a bug. KVM
 * compares task_pid(current) with the pid that last ran a vCPU to notice the
 * first run and a move to another thread.
 */
static inline struct pid *get_pid(struct pid *p) { return p; }
static inline void put_pid(struct pid *p) { (void)p; }
static inline pid_t pid_vnr(struct pid *p) { return p ? p->nr : 0; }
static inline struct pid *task_pid(void *task)
{ return task ? &((struct lkpi_task *)task)->pid_obj : 0; }
static inline struct pid *task_tgid(void *task) { return task_pid(task); }
static inline pid_t task_pid_vnr(void *task)
{ return task ? ((struct lkpi_task *)task)->pid : 0; }
static inline pid_t task_pid_nr(void *task) { return task_pid_vnr(task); }

#define pid_nr(p) ((p) ? (p)->nr : 0)

/* The task behind a pid handle: the one it is part of. */
static inline struct lkpi_task *pid_task(struct pid *pid, int type)
{
	(void)type;
	return pid ? (struct lkpi_task *)((char *)pid - __builtin_offsetof(struct lkpi_task, pid_obj)) : 0;
}

#endif
