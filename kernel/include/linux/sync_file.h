/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SYNC_FILE_H
#define LKPI_LINUX_SYNC_FILE_H
#include <linux/dma-fence.h>
#include <linux/file.h>
#include <linux/wait.h>
/* A fence wrapped in a descriptor, so userspace can wait on GPU work: poll it
 * for completion, ask it for its state, merge two into one. */
struct sync_file {
	struct file *file;
	struct dma_fence *fence;
	wait_queue_head_t wq;
	unsigned long flags;
	struct dma_fence_cb cb;
	char user_name[32];
};
extern const struct file_operations sync_file_fops;
struct sync_file *sync_file_create(struct dma_fence *fence);
struct dma_fence *sync_file_get_fence(int fd);

/* A sync_file wraps a fence that is often an array of them, and callers reach
 * the array interface through this header the way upstream's chain lets them. */
#include <linux/dma-fence-array.h>

#endif
