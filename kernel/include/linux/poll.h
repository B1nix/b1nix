/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_POLL_H
#define LKPI_LINUX_POLL_H
#include <linux/fs.h>
#include <linux/wait.h>
#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define EPOLLIN  POLLIN
#define EPOLLOUT POLLOUT
#define EPOLLERR POLLERR
#define EPOLLHUP POLLHUP
#define POLLRDNORM  0x0040
#define POLLWRNORM  0x0100
#define EPOLLRDNORM POLLRDNORM
#define EPOLLWRNORM POLLWRNORM
#define EPOLLPRI    0x0002
typedef unsigned int __poll_t;
/* b1nix parks a poller on its own poll channel, so there is nothing to
 * register -- but the queue must learn that it is polled, or its wake_up
 * never reaches that channel. Stored before the caller tests readiness, and
 * read by wake_up after the event is published, so no wake falls between. */
static inline void poll_wait(struct file *f, struct wait_queue_head *wq,
                             poll_table *p)
{
	(void)f;
	(void)p;
	if (wq)
		__atomic_store_n(&wq->polled, 1u, __ATOMIC_SEQ_CST);
}
#endif
