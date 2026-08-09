/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SYNC_FILE_H
#define LKPI_LINUX_SYNC_FILE_H
#include <linux/dma-fence.h>
#include <linux/file.h>
/* A fence wrapped in a descriptor, so userspace can wait on GPU work. Needs the
 * anonymous-inode path first; declared here, wired when that lands. */
struct sync_file { struct file *file; struct dma_fence *fence; };
struct sync_file *sync_file_create(struct dma_fence *fence);
struct dma_fence *sync_file_get_fence(int fd);
#endif
