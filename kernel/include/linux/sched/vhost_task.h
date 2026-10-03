/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SCHED_VHOST_TASK_H
#define LKPI_LINUX_SCHED_VHOST_TASK_H
#include <linux/types.h>
/* A worker owned by a process: it runs fn(data) each time it is woken until
 * fn returns false and nothing is pending, and stops when told to or when its
 * owner is killed. Here it is a kernel thread (lkpi). */
struct vhost_task;
struct vhost_task *vhost_task_create(bool (*fn)(void *),
				     void (*handle_sigkill)(void *), void *arg,
				     const char *name);
void vhost_task_start(struct vhost_task *vtsk);
void vhost_task_stop(struct vhost_task *vtsk);
void vhost_task_wake(struct vhost_task *vtsk);
bool vhost_task_should_stop(struct vhost_task *vtsk);
#endif
