/* M47 — /dev/fb0: mmap-able linear framebuffer device.
 *
 * The device exposes a kernel-owned, physically contiguous shadow buffer
 * (32bpp, pitch = width*4). Userspace mmaps it (MAP_SHARED; sys_mmap maps
 * the frames directly via mmap_phys_cb with VMM_SHARED + a pmm ref per
 * mapping, the SysV-shm pattern) and pushes dirty rectangles with
 * B1NIX_FBIOFLUSH. The flush path mirrors the kernel compositor: prefer the
 * virtio-gpu transfer+flush, fall back to a row copy into the boot
 * framebuffer.
 *
 * The shadow buffer remains kernel-owned, while mmap_open/mmap_close hooks
 * count live userspace VMAs. The first mapping claims scanout from the kernel
 * compositor; the last unmap or process exit returns it and requests a full
 * kernel redraw. */
#include <b1nix/compositor.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/fb.h>
#include <b1nix/fb_console.h>
#include <b1nix/mm.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <b1nix/virtio_gpu.h>
#include <string.h>

static u32 *fb_shadow;     /* direct-map virtual address */
static u64 fb_shadow_phys; /* contiguous physical base */
static usize fb_shadow_size;
static unsigned fb_mapping_count;
static spinlock_t fb_lock;

int fb_dev_claimed(void) {
  return __atomic_load_n(&fb_mapping_count, __ATOMIC_ACQUIRE) != 0;
}

/* Allocate the contiguous shadow buffer on first use. Returns 0 or -errno.
 * Called with fb_lock held. */
static int fb_shadow_ensure(void) {
  if (fb_shadow)
    return 0;
  usize size = (usize)fb_console_width() * (usize)fb_console_height() * 4u;
  usize frames = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  u64 phys = pmm_alloc_frames(frames);
  if (!phys)
    return -ENOMEM;
  fb_shadow_phys = phys;
  fb_shadow_size = frames * PAGE_SIZE;
  fb_shadow = (u32 *)(usize)(phys + vmm_direct_map_base());
  memset(fb_shadow, 0, fb_shadow_size);
  return 0;
}

static int fb_mmap_phys(struct vfs_node *node, u64 offset, usize length,
                        u64 *out_phys) {
  (void)node;
  u64 flags;
  spin_lock_irqsave(&fb_lock, &flags);
  int rc = fb_shadow_ensure();
  if (rc == 0) {
    if (offset + length > fb_shadow_size) {
      rc = -EINVAL;
    } else {
      *out_phys = fb_shadow_phys + offset;
    }
  }
  spin_unlock_irqrestore(&fb_lock, flags);
  return rc;
}

void fb_dev_mapping_open(struct vfs_node *node) {
  (void)node;
  int first_claim;
  u64 flags;
  spin_lock_irqsave(&fb_lock, &flags);
  first_claim = fb_mapping_count++ == 0;
  spin_unlock_irqrestore(&fb_lock, flags);
  if (first_claim)
    console_write("fb0: claimed by userspace mapping\n");
}

void fb_dev_mapping_close(struct vfs_node *node) {
  (void)node;
  int released = 0;
  u64 flags;
  spin_lock_irqsave(&fb_lock, &flags);
  if (fb_mapping_count > 0) {
    fb_mapping_count--;
    released = fb_mapping_count == 0;
  }
  spin_unlock_irqrestore(&fb_lock, flags);
  if (released) {
    console_write("fb0: released to kernel console\n");
    compositor_reclaim_display();
  }
}

static int fb_flush_rect(struct b1nix_fb_rect *r) {
  u32 width = fb_console_width();
  u32 height = fb_console_height();
  if (r->x >= width || r->y >= height || r->w == 0 || r->h == 0)
    return -EINVAL;
  if (r->x + r->w > width)
    r->w = width - r->x;
  if (r->y + r->h > height)
    r->h = height - r->y;

  if (virtio_gpu_ready()) {
    if (virtio_gpu_present(fb_shadow, width, height, r->x, r->y, r->w, r->h,
                           0, 0, 0) == 0)
      return 0;
  }

  volatile u8 *front = (volatile u8 *)fb_console_frontbuffer();
  if (!front)
    return -EIO;
  u32 pitch = fb_console_pitch();
  for (u32 y = r->y; y < r->y + r->h; y++) {
    volatile u32 *dst = (volatile u32 *)(front + (u64)y * pitch) + r->x;
    u32 *src = fb_shadow + (u64)y * width + r->x;
    for (u32 x = 0; x < r->w; x++)
      dst[x] = src[x];
  }
  return 0;
}

static int fb_ioctl(struct vfs_node *node, u64 request, void *arg) {
  (void)node;
  switch (request) {
  case B1NIX_FBIOGET_INFO: {
    struct b1nix_fb_info info;
    info.width = fb_console_width();
    info.height = fb_console_height();
    info.pitch = fb_console_width() * 4u;
    info.bpp = 32;
    if (!arg || syscall_copyout(arg, &info, sizeof(info)) < 0)
      return -EFAULT;
    return 0;
  }
  case B1NIX_FBIOFLUSH: {
    struct b1nix_fb_rect rect;
    if (!arg || syscall_copyin(&rect, arg, sizeof(rect)) < 0)
      return -EFAULT;
    u64 flags;
    spin_lock_irqsave(&fb_lock, &flags);
    int rc = fb_shadow_ensure();
    spin_unlock_irqrestore(&fb_lock, flags);
    if (rc < 0)
      return rc;
    /* The flush copies from the (stable, kernel-owned) shadow without the
     * lock: a concurrent userspace draw only tears pixels, never memory. */
    return fb_flush_rect(&rect);
  }
  default:
    return -ENOTTY;
  }
}

void fb_dev_init(void) {
  if (!fb_console_ready() || fb_console_bpp() != 32) {
    console_write("fb0: disabled (no 32bpp boot framebuffer)\n");
    return;
  }
  struct vfs_node *node = vfs_add_node("/dev/fb0", VFS_DEVICE, 0, 0, 0);
  if (!node || IS_ERR(node)) {
    console_write("fb0: failed to register /dev/fb0\n");
    return;
  }
  node->inode->mode = 0600;
  node->inode->ioctl_cb = fb_ioctl;
  node->inode->mmap_phys_cb = fb_mmap_phys;
  node->inode->mmap_open_cb = fb_dev_mapping_open;
  node->inode->mmap_close_cb = fb_dev_mapping_close;
  vfs_node_put(node);
  console_write("fb0: ready ");
  console_write_dec(fb_console_width());
  console_putc('x');
  console_write_dec(fb_console_height());
  console_write("x32\n");
}
