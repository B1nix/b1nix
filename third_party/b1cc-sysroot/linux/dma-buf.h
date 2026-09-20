#ifndef B1NIX_U_LINUX_DMA_BUF_H
#define B1NIX_U_LINUX_DMA_BUF_H
/* Linux dma-buf CPU-access sync UAPI. b1nix has no dma-buf; provided so the
 * dmabuf-backed native pixmap code (ui/gfx/linux) compiles. */
#include <linux/types.h>
#include <sys/ioctl.h>

struct dma_buf_sync {
  __u64 flags;
};

#define DMA_BUF_SYNC_READ  (1 << 0)
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_RW    (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END   (1 << 2)

#define DMA_BUF_BASE       'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

#endif /* B1NIX_U_LINUX_DMA_BUF_H */
