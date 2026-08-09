/* SPDX-License-Identifier: MIT */
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
static inline void poll_wait(struct file *f, struct wait_queue_head *wq,
                             poll_table *p)
{ (void)f; (void)wq; (void)p; }
#endif
