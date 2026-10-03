/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_EVENTFD_H
#define LKPI_LINUX_EVENTFD_H
#include <linux/file.h>
#include <linux/types.h>
#include <linux/wait.h>

/*
 * A process's eventfd, as kernel code sees it (M131: KVM's irqfd and
 * ioeventfd). The counter is b1nix's own eventfd; this is the side a driver
 * signals, reads and hangs a wait-queue callback on. There is one per eventfd,
 * made on first use and freed with the eventfd itself, so two lookups of one
 * descriptor compare equal, as KVM's duplicate checks need.
 *
 * A reference (eventfd_ctx_fdget/_fileget, dropped by eventfd_ctx_put) holds
 * the eventfd open. That is where this differs from Linux: userspace closing
 * its last descriptor does not hang the kernel user up (no EPOLLHUP), which
 * for KVM means an irqfd lives until it is deassigned or the VM goes.
 */
struct eventfd_ctx {
	void *handle;           /* the b1nix eventfd; references are on it */
	wait_queue_head_t wqh;  /* kernel waiters, woken with EPOLLIN */
};

struct eventfd_ctx *eventfd_ctx_fdget(int fd);
struct eventfd_ctx *eventfd_ctx_fileget(struct file *file);
void eventfd_ctx_put(struct eventfd_ctx *ctx);
void eventfd_signal(struct eventfd_ctx *ctx);
void eventfd_ctx_do_read(struct eventfd_ctx *ctx, __u64 *cnt);
int eventfd_ctx_remove_wait_queue(struct eventfd_ctx *ctx, wait_queue_entry_t *wait,
				  __u64 *cnt);

/* fget() of an eventfd descriptor: a file standing for it, holding one
 * reference, whose private_data is the eventfd_ctx. */
struct file *lkpi_eventfd_file(int fd);
bool lkpi_file_is_eventfd(const struct file *f);
#endif
