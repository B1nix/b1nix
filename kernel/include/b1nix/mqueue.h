/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_MQUEUE_H
#define B1NIX_MQUEUE_H

#include <b1nix/types.h>

/* POSIX message queues (kernel/ipc/mqueue.c).
 *
 * A queue is a file in its IPC namespace's mqueue filesystem, and a message
 * queue descriptor is an ordinary descriptor on that file: close(2), poll(2),
 * dup(2), fork and exec all work on it the way they do on Linux. Mounting
 * "mqueue" shows the queues of the mounting task's IPC namespace.
 *
 * Limits are Linux's defaults (/proc/sys/fs/mqueue): an unprivileged caller
 * may ask for at most MQ_MSG_MAX messages of MQ_MSGSIZE_MAX bytes; with
 * CAP_SYS_RESOURCE the hard limits apply. */

#define MQ_PRIO_MAX           32768
#define MQ_MSG_DEFAULT        10
#define MQ_MSGSIZE_DEFAULT    8192
#define MQ_MSG_MAX            10
#define MQ_MSGSIZE_MAX        8192
#define MQ_MSG_HARDMAX        65536
#define MQ_MSGSIZE_HARDMAX    (16 * 1024 * 1024)
#define MQ_QUEUES_MAX         256

void mqueue_init(void);

/* The Linux system calls, with Linux argument layouts. `oflag` for mq_open is
 * already in the kernel's O_* encoding. */
int mqueue_open(const char *name, int oflag, u32 mode, const void *user_attr);
int mqueue_unlink(const char *name);
isize mqueue_timedsend(int fd, const void *user_msg, usize len, u32 prio,
                       const void *user_timeout);
isize mqueue_timedreceive(int fd, void *user_msg, usize len, void *user_prio,
                          const void *user_timeout);
int mqueue_notify(int fd, const void *user_sigevent);
int mqueue_getsetattr(int fd, const void *user_new, void *user_old);

/* Drop every queue of an IPC namespace that is going away. */
void mqueue_ns_destroy(u32 ns);

#endif /* B1NIX_MQUEUE_H */
