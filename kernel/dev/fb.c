/* M47 — /dev/fb0: mmap-able linear framebuffer device.
 *
 * The device exposes a kernel-owned, physically contiguous shadow buffer
 * (32bpp, pitch = width*4). Userspace mmaps it (MAP_SHARED; sys_mmap maps
 * the frames directly via mmap_phys_cb with VMM_SHARED + a pmm ref per
 * mapping, the SysV-shm pattern) and pushes dirty rectangles with
 * B1NIX_FBIOFLUSH. The flush path prefers the virtio-gpu transfer+flush,
 * falling back to a row copy into the boot framebuffer.
 *
 * The shadow buffer remains kernel-owned, while mmap_open/mmap_close hooks
 * count live userspace VMAs. The first mapping claims scanout; the last
 * unmap or process exit returns it and requests a full kernel redraw. */
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
  if (released)
    console_write("fb0: released to kernel console\n");
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

/* Linux's own framebuffer interface, so fbset and anything else written for
 * /dev/fb0 works without a b1nix-specific code path. Only the two GET ioctls
 * exist: the mode is fixed by whatever the bootloader handed us, so accepting
 * FBIOPUT_VSCREENINFO would mean reporting a mode change that never happened. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield_u {
  u32 offset;
  u32 length;
  u32 msb_right;
};

struct fb_var_screeninfo_u {
  u32 xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
  u32 bits_per_pixel, grayscale;
  struct fb_bitfield_u red, green, blue, transp;
  u32 nonstd, activate, height, width, accel_flags;
  u32 pixclock, left_margin, right_margin, upper_margin, lower_margin;
  u32 hsync_len, vsync_len, sync, vmode, rotate, colorspace;
  u32 reserved[4];
};

struct fb_fix_screeninfo_u {
  char id[16];
  unsigned long smem_start;
  u32 smem_len;
  u32 type, type_aux, visual;
  u16 xpanstep, ypanstep, ywrapstep;
  u32 line_length;
  unsigned long mmio_start;
  u32 mmio_len;
  u32 accel;
  u16 capabilities;
  u16 reserved[2];
};

static int fb_ioctl(struct vfs_node *node, u64 request, void *arg) {
  (void)node;
  switch (request) {
  case FBIOGET_VSCREENINFO: {
    struct fb_var_screeninfo_u v;
    memset(&v, 0, sizeof(v));
    v.xres = v.xres_virtual = fb_console_width();
    v.yres = v.yres_virtual = fb_console_height();
    v.bits_per_pixel = 32;
    /* XRGB8888, which is what fb_console renders and what the shadow holds. */
    v.red.offset = 16; v.red.length = 8;
    v.green.offset = 8; v.green.length = 8;
    v.blue.offset = 0; v.blue.length = 8;
    v.transp.offset = 24; v.transp.length = 0;
    if (!arg || syscall_copyout(arg, &v, sizeof(v)) < 0)
      return -EFAULT;
    return 0;
  }
  case FBIOGET_FSCREENINFO: {
    struct fb_fix_screeninfo_u f;
    memset(&f, 0, sizeof(f));
    const char *id = "b1nix-fb";
    for (int i = 0; id[i] && i < 15; i++)
      f.id[i] = id[i];
    f.smem_len = fb_console_width() * fb_console_height() * 4u;
    f.type = 0;   /* FB_TYPE_PACKED_PIXELS */
    f.visual = 2; /* FB_VISUAL_TRUECOLOR */
    f.line_length = fb_console_width() * 4u;
    if (!arg || syscall_copyout(arg, &f, sizeof(f)) < 0)
      return -EFAULT;
    return 0;
  }
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
  case B1NIX_FBIOGET_CLAIM: {
    int claimed = fb_dev_claimed();
    if (!arg || syscall_copyout(arg, &claimed, sizeof(claimed)) < 0)
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
