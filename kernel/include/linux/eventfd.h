/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_EVENTFD_H
#define LKPI_LINUX_EVENTFD_H
#include <linux/file.h>
/* A counter a descriptor can be woken on. Used to signal userspace from a
 * driver; wired up with anon_inodes. */
struct eventfd_ctx;
struct eventfd_ctx *eventfd_ctx_fdget(int fd);
void eventfd_ctx_put(struct eventfd_ctx *ctx);
void eventfd_signal(struct eventfd_ctx *ctx, u64 n);
#endif
