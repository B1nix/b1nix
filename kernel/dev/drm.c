/*
 * DRM/KMS device over virtio-gpu.
 *
 * M50 shipped this as a single ioctl switch over eight commands with buffer
 * objects allocated as one physically contiguous run. M100 changes two things
 * and nothing else about the behaviour userspace sees:
 *
 *   1. Buffer objects are scatter-gather backed (kernel/include/lkpi/
 *      scatterlist.h). A contiguous multi-megabyte allocation is the first
 *      thing that fails on a fragmented system, and no real GPU driver needs
 *      one — it needs a page list. Pages are mapped into a per-object kernel
 *      window so the scanout path still sees a linear buffer, and into
 *      userspace one page at a time through the new per-page mmap callback.
 *
 *   2. The ioctl switch is split into one handler per command. The switch had
 *      grown to cyclomatic 56 / cognitive 161, which is already past the point
 *      where a reviewer can hold it in their head, and the driver has a lot
 *      more commands ahead of it.
 */

#include <b1nix/console.h>
#include <stdio.h>
#include <b1nix/drm.h>
#include <b1nix/sysfs_attr.h>
#include <b1nix/uevent.h>
#include <b1nix/errno.h>
#include <b1nix/fb.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <b1nix/virtio_gpu.h>
#include <lkpi/idr.h>
#include <lkpi/scatterlist.h>
#include <string.h>

#define DRM_CRTC_ID 1
#define DRM_CONNECTOR_ID 1
#define DRM_ENCODER_ID 1
#define DRM_MAX_OBJECTS 8
#define DRM_MAP_STRIDE (64ULL * 1024 * 1024)
/* Kernel virtual window used to give each scatter-gather object a linear view.
 * One DRM_MAP_STRIDE slot per object, so slot i is always at the same address
 * and no VA allocator is needed. Placed above the MMIO window (which starts at
 * 0xffffa000_00000000 and is 512 MiB long) with a terabyte of clearance. */
#define DRM_VMAP_BASE 0xffffa10000000000ULL
#define DRM_MAX_BO_PAGES (DRM_MAP_STRIDE / PAGE_SIZE)
/* The whole window: one stride per object. Reserved as a unit at init so every
 * slot's page-table path exists before any address space is created. */
#define DRM_VMAP_WINDOW_TOTAL (DRM_MAP_STRIDE * DRM_MAX_OBJECTS)

struct drm_client {
  struct idr handles;
  u32 next_fb_id;
  u32 sequence;
  int event_pending;
  struct drm_event_vblank event;
};

struct drm_buffer {
  struct drm_client *owner;
  struct sg_table sgt;
  u64 *frames;       /* one entry per page, in object order */
  u32 npages;
  u32 *virt;         /* linear kernel view of the object */
  usize size;
  u32 width, height, pitch, handle;
  u32 map_refs, present_refs;
  int pending_free;
  int in_use;
};

struct drm_fb {
  struct drm_client *owner;
  struct drm_buffer *buffer;
  u32 id;
};

static struct drm_buffer buffers[DRM_MAX_OBJECTS];
static struct drm_fb fbs[DRM_MAX_OBJECTS];
static struct drm_fb *active_fb;
static struct vfs_node *card_node;

/* ── scatter-gather buffer objects ──────────────────────────────── */

static u64 drm_slot_vaddr(int slot) {
  return DRM_VMAP_BASE + (u64)slot * DRM_MAP_STRIDE;
}

static u64 drm_slot_mmap_offset(int slot) {
  return (u64)(slot + 1) * DRM_MAP_STRIDE;
}

/* Allocate `npages` single frames, describe them with an sg table and map them
 * contiguously into this slot's kernel window. Frames are taken one at a time
 * on purpose: that is the allocation a fragmented system can actually satisfy,
 * and it is what exercises the scatter-gather path. */
static int drm_bo_alloc(struct drm_buffer *bo, int slot, u32 npages) {
  if (npages == 0 || npages > DRM_MAX_BO_PAGES)
    return -EINVAL;

  bo->frames = kzalloc((usize)npages * sizeof(u64));
  if (!bo->frames)
    return -ENOMEM;

  u32 got = 0;
  for (; got < npages; got++) {
    bo->frames[got] = pmm_alloc_frame();
    if (!bo->frames[got])
      break;
  }
  if (got != npages) {
    for (u32 i = 0; i < got; i++)
      pmm_free_frame(bo->frames[i]);
    kfree(bo->frames);
    bo->frames = 0;
    return -ENOMEM;
  }

  if (sg_alloc_table_from_pages(&bo->sgt, bo->frames, npages) < 0) {
    for (u32 i = 0; i < npages; i++)
      pmm_free_frame(bo->frames[i]);
    kfree(bo->frames);
    bo->frames = 0;
    return -ENOMEM;
  }

  u64 base = drm_slot_vaddr(slot);
  for (u32 i = 0; i < npages; i++)
    vmm_map_page(base + (u64)i * PAGE_SIZE, bo->frames[i],
                 VMM_WRITABLE | VMM_NO_EXECUTE | VMM_PRESENT);

  bo->npages = npages;
  bo->virt = (u32 *)(usize)base;
  bo->size = (usize)npages * PAGE_SIZE;
  memset(bo->virt, 0, bo->size);
  return 0;
}

static void drm_bo_free(struct drm_buffer *bo) {
  int slot = (int)(bo - buffers);
  u64 base = drm_slot_vaddr(slot);
  for (u32 i = 0; i < bo->npages; i++)
    vmm_unmap_page(base + (u64)i * PAGE_SIZE);
  for (u32 i = 0; i < bo->npages; i++)
    pmm_free_frame(bo->frames[i]);
  sg_free_table(&bo->sgt);
  if (bo->frames)
    kfree(bo->frames);
  memset(bo, 0, sizeof(*bo));
}

static struct drm_buffer *find_buffer(struct drm_client *client, u32 handle) {
  if (!client)
    return 0;
  struct drm_buffer *bo = idr_find(&client->handles, handle);
  return (bo && bo->owner == client) ? bo : 0;
}

static struct drm_buffer *find_mapped_buffer(u64 offset, usize length) {
  for (int i = 0; i < DRM_MAX_OBJECTS; i++) {
    u64 base = drm_slot_mmap_offset(i);
    if (buffers[i].in_use && offset >= base &&
        offset - base <= buffers[i].size &&
        length <= buffers[i].size - (usize)(offset - base))
      return &buffers[i];
  }
  return 0;
}

static struct drm_fb *find_fb(struct drm_client *client, u32 id) {
  for (int i = 0; i < DRM_MAX_OBJECTS; i++)
    if (fbs[i].owner == client && fbs[i].id == id)
      return &fbs[i];
  return 0;
}

static int buffer_has_fb(struct drm_buffer *buffer) {
  for (int i = 0; i < DRM_MAX_OBJECTS; i++)
    if (fbs[i].buffer == buffer)
      return 1;
  return 0;
}

static void maybe_free_buffer(struct drm_buffer *buffer) {
  if (buffer->pending_free && !buffer->map_refs && !buffer->present_refs)
    drm_bo_free(buffer);
}

static void drm_mode(struct drm_mode_modeinfo *mode) {
  u32 width = 0, height = 0;
  virtio_gpu_get_mode(&width, &height);
  memset(mode, 0, sizeof(*mode));
  mode->hdisplay = (u16)width;
  mode->vdisplay = (u16)height;
  mode->vrefresh = 60;
  strcpy(mode->name, "virtio");
}

static int present_fb(struct drm_fb *fb) {
  struct drm_buffer *buffer = fb ? fb->buffer : 0;
  if (!buffer || !buffer->virt)
    return -EINVAL;
  buffer->present_refs++;
  int rc = virtio_gpu_present(buffer->virt, buffer->width, buffer->height,
                              0, 0, buffer->width, buffer->height,
                              0, 0, 0) < 0
               ? -EIO
               : 0;
  buffer->present_refs--;
  maybe_free_buffer(buffer);
  return rc;
}

/* ── file operations ────────────────────────────────────────────── */

static isize drm_read(struct vfs_handle *h, char *buf, usize len) {
  struct drm_client *client = h->private_data;
  if (!client || len < sizeof(client->event))
    return -EINVAL;
  while (!client->event_pending) {
    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    scheduler_block_on(&client->event_pending);
  }
  memcpy(buf, &client->event, sizeof(client->event));
  client->event_pending = 0;
  return sizeof(client->event);
}

static int drm_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct drm_client *client = h->private_data;
  pfd->revents = client && client->event_pending ? B1NIX_POLLIN : 0;
  return 0;
}

/* ── per-command ioctl handlers ─────────────────────────────────────
 *
 * Each takes the calling client and the user pointer, copies its own argument
 * in and out, and returns 0 or a negative errno. Behaviour is unchanged from
 * the single switch these were split out of.
 */

static int drm_ioctl_getresources(struct drm_client *client, void *arg) {
  struct drm_mode_card_res r;
  if (syscall_copyin(&r, arg, sizeof(r)) < 0)
    return -EFAULT;
  u32 id = DRM_CRTC_ID;
  if (r.crtc_id_ptr && r.count_crtcs &&
      syscall_copyout((void *)(usize)r.crtc_id_ptr, &id, sizeof(id)) < 0)
    return -EFAULT;
  id = DRM_CONNECTOR_ID;
  if (r.connector_id_ptr && r.count_connectors &&
      syscall_copyout((void *)(usize)r.connector_id_ptr, &id, sizeof(id)) < 0)
    return -EFAULT;
  id = DRM_ENCODER_ID;
  if (r.encoder_id_ptr && r.count_encoders &&
      syscall_copyout((void *)(usize)r.encoder_id_ptr, &id, sizeof(id)) < 0)
    return -EFAULT;

  u32 fb_ids[DRM_MAX_OBJECTS];
  u32 count = 0;
  for (int i = 0; i < DRM_MAX_OBJECTS; i++)
    if (fbs[i].owner == client)
      fb_ids[count++] = fbs[i].id;
  if (r.fb_id_ptr && r.count_fbs) {
    u32 n = r.count_fbs < count ? r.count_fbs : count;
    if (syscall_copyout((void *)(usize)r.fb_id_ptr, fb_ids,
                        (usize)n * sizeof(u32)) < 0)
      return -EFAULT;
  }

  u32 width = 0, height = 0;
  virtio_gpu_get_mode(&width, &height);
  r.count_fbs = count;
  r.count_crtcs = r.count_connectors = r.count_encoders = 1;
  r.min_width = r.min_height = 1;
  r.max_width = width;
  r.max_height = height;
  return syscall_copyout(arg, &r, sizeof(r)) < 0 ? -EFAULT : 0;
}

static int drm_ioctl_getconnector(void *arg) {
  struct drm_mode_get_connector c;
  if (syscall_copyin(&c, arg, sizeof(c)) < 0 ||
      c.connector_id != DRM_CONNECTOR_ID)
    return -EINVAL;
  if (c.modes_ptr && c.count_modes) {
    struct drm_mode_modeinfo mode;
    drm_mode(&mode);
    if (syscall_copyout((void *)(usize)c.modes_ptr, &mode, sizeof(mode)) < 0)
      return -EFAULT;
  }
  if (c.encoders_ptr && c.count_encoders) {
    u32 id = DRM_ENCODER_ID;
    if (syscall_copyout((void *)(usize)c.encoders_ptr, &id, sizeof(id)) < 0)
      return -EFAULT;
  }
  c.count_modes = c.count_encoders = 1;
  c.count_props = 0;
  c.encoder_id = DRM_ENCODER_ID;
  c.connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
  c.connector_type_id = 1;
  c.connection = DRM_MODE_CONNECTED;
  return syscall_copyout(arg, &c, sizeof(c)) < 0 ? -EFAULT : 0;
}

static int drm_ioctl_create_dumb(struct drm_client *client, void *arg) {
  struct drm_mode_create_dumb d;
  if (syscall_copyin(&d, arg, sizeof(d)) < 0)
    return -EFAULT;
  if (!d.width || !d.height || d.bpp != 32 || d.width > 65535 ||
      d.height > 65535)
    return -EINVAL;
  u64 bytes = (u64)d.width * d.height * 4;
  if (bytes > DRM_MAP_STRIDE)
    return -ENOMEM;

  int slot = -1;
  for (int i = 0; i < DRM_MAX_OBJECTS; i++)
    if (!buffers[i].in_use) {
      slot = i;
      break;
    }
  if (slot < 0)
    return -ENOSPC;

  struct drm_buffer *buffer = &buffers[slot];
  u32 npages = (u32)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
  memset(buffer, 0, sizeof(*buffer));
  buffer->in_use = 1;
  int rc = drm_bo_alloc(buffer, slot, npages);
  if (rc < 0) {
    memset(buffer, 0, sizeof(*buffer));
    return rc;
  }

  buffer->owner = client;
  buffer->width = d.width;
  buffer->height = d.height;
  buffer->pitch = d.width * 4;
  buffer->in_use = 1;

  int handle = idr_alloc(&client->handles, buffer, 1, 0);
  if (handle < 0) {
    drm_bo_free(buffer);
    return handle;
  }
  buffer->handle = (u32)handle;

  d.handle = buffer->handle;
  d.pitch = buffer->pitch;
  d.size = buffer->size;
  if (syscall_copyout(arg, &d, sizeof(d)) < 0) {
    idr_remove(&client->handles, buffer->handle);
    buffer->owner = 0;
    buffer->pending_free = 1;
    maybe_free_buffer(buffer);
    return -EFAULT;
  }
  return 0;
}

static int drm_ioctl_map_dumb(struct drm_client *client, void *arg) {
  struct drm_mode_map_dumb d;
  if (syscall_copyin(&d, arg, sizeof(d)) < 0)
    return -EFAULT;
  struct drm_buffer *buffer = find_buffer(client, d.handle);
  if (!buffer)
    return -EINVAL;
  d.offset = drm_slot_mmap_offset((int)(buffer - buffers));
  return syscall_copyout(arg, &d, sizeof(d)) < 0 ? -EFAULT : 0;
}

static int drm_ioctl_addfb(struct drm_client *client, void *arg) {
  struct drm_mode_fb_cmd f;
  if (syscall_copyin(&f, arg, sizeof(f)) < 0)
    return -EFAULT;
  struct drm_buffer *buffer = find_buffer(client, f.handle);
  if (!buffer || f.width != buffer->width || f.height != buffer->height ||
      f.pitch != buffer->pitch || f.bpp != 32)
    return -EINVAL;
  int slot = -1;
  for (int i = 0; i < DRM_MAX_OBJECTS; i++)
    if (!fbs[i].owner) {
      slot = i;
      break;
    }
  if (slot < 0)
    return -ENOSPC;
  fbs[slot].owner = client;
  fbs[slot].buffer = buffer;
  fbs[slot].id = client->next_fb_id++;
  f.fb_id = fbs[slot].id;
  if (syscall_copyout(arg, &f, sizeof(f)) < 0) {
    memset(&fbs[slot], 0, sizeof(fbs[slot]));
    return -EFAULT;
  }
  return 0;
}

static int drm_ioctl_rmfb(struct drm_client *client, void *arg) {
  u32 id;
  if (syscall_copyin(&id, arg, sizeof(id)) < 0)
    return -EFAULT;
  struct drm_fb *fb = find_fb(client, id);
  if (!fb)
    return -EINVAL;
  if (active_fb == fb)
    active_fb = 0;
  memset(fb, 0, sizeof(*fb));
  return 0;
}

static int drm_ioctl_setcrtc(struct drm_client *client, void *arg) {
  struct drm_mode_crtc c;
  if (syscall_copyin(&c, arg, sizeof(c)) < 0 || c.crtc_id != DRM_CRTC_ID)
    return -EINVAL;
  struct drm_fb *fb = find_fb(client, c.fb_id);
  if (!fb || (c.mode_valid && (c.mode.hdisplay != fb->buffer->width ||
                               c.mode.vdisplay != fb->buffer->height)))
    return -EINVAL;
  if (c.count_connectors) {
    u32 connector;
    if (!c.set_connectors_ptr ||
        syscall_copyin(&connector, (void *)(usize)c.set_connectors_ptr,
                       sizeof(connector)) < 0 ||
        connector != DRM_CONNECTOR_ID)
      return -EINVAL;
  }
  int rc = present_fb(fb);
  if (!rc)
    active_fb = fb;
  return rc;
}

static int drm_ioctl_page_flip(struct drm_client *client, void *arg) {
  struct drm_mode_crtc_page_flip f;
  if (syscall_copyin(&f, arg, sizeof(f)) < 0 || f.crtc_id != DRM_CRTC_ID ||
      (f.flags & ~DRM_MODE_PAGE_FLIP_EVENT))
    return -EINVAL;
  struct drm_fb *fb = find_fb(client, f.fb_id);
  if (!fb)
    return -EINVAL;
  int rc = present_fb(fb);
  if (rc)
    return rc;
  active_fb = fb;
  if (f.flags & DRM_MODE_PAGE_FLIP_EVENT) {
    memset(&client->event, 0, sizeof(client->event));
    client->event.base.type = DRM_EVENT_FLIP_COMPLETE;
    client->event.base.length = sizeof(client->event);
    client->event.user_data = f.user_data;
    client->event.sequence = ++client->sequence;
    client->event.crtc_id = DRM_CRTC_ID;
    client->event_pending = 1;
    scheduler_wake_all(&client->event_pending);
    scheduler_wake_all(vfs_poll_chan);
  }
  return 0;
}

static int drm_ioctl_destroy_dumb(struct drm_client *client, void *arg) {
  struct drm_mode_destroy_dumb d;
  if (syscall_copyin(&d, arg, sizeof(d)) < 0)
    return -EFAULT;
  struct drm_buffer *buffer = find_buffer(client, d.handle);
  if (!buffer)
    return -EINVAL;
  if (buffer_has_fb(buffer))
    return -EBUSY;
  idr_remove(&client->handles, d.handle);
  buffer->owner = 0;
  buffer->pending_free = 1;
  maybe_free_buffer(buffer);
  return 0;
}

static int drm_ioctl_gem_info(struct drm_client *client, void *arg) {
  struct drm_b1nix_gem_info gi;
  if (syscall_copyin(&gi, arg, sizeof(gi)) < 0)
    return -EFAULT;
  struct drm_buffer *buffer = find_buffer(client, gi.handle);
  if (!buffer)
    return -EINVAL;
  gi.nents = buffer->sgt.nents;
  gi.npages = buffer->npages;
  gi.contiguous = (u32)sg_is_contiguous(&buffer->sgt);
  gi.size = buffer->size;
  return syscall_copyout(arg, &gi, sizeof(gi)) < 0 ? -EFAULT : 0;
}

static int drm_ioctl(struct vfs_handle *h, u64 request, void *arg) {
  struct drm_client *client = h->private_data;
  if (!client || !arg)
    return -EFAULT;

  switch ((u32)request) {
  case DRM_IOCTL_MODE_GETRESOURCES:
    return drm_ioctl_getresources(client, arg);
  case DRM_IOCTL_MODE_GETCONNECTOR:
    return drm_ioctl_getconnector(arg);
  case DRM_IOCTL_MODE_CREATE_DUMB:
    return drm_ioctl_create_dumb(client, arg);
  case DRM_IOCTL_MODE_MAP_DUMB:
    return drm_ioctl_map_dumb(client, arg);
  case DRM_IOCTL_MODE_ADDFB:
    return drm_ioctl_addfb(client, arg);
  case DRM_IOCTL_MODE_RMFB:
    return drm_ioctl_rmfb(client, arg);
  case DRM_IOCTL_MODE_SETCRTC:
    return drm_ioctl_setcrtc(client, arg);
  case DRM_IOCTL_MODE_PAGE_FLIP:
    return drm_ioctl_page_flip(client, arg);
  case DRM_IOCTL_MODE_DESTROY_DUMB:
    return drm_ioctl_destroy_dumb(client, arg);
  case DRM_IOCTL_B1NIX_GEM_INFO:
    return drm_ioctl_gem_info(client, arg);
  default:
    return -ENOTTY;
  }
}

static void drm_release(struct vfs_handle *h) {
  struct drm_client *client = h->private_data;
  if (client) {
    for (int i = 0; i < DRM_MAX_OBJECTS; i++) {
      if (fbs[i].owner == client) {
        if (active_fb == &fbs[i])
          active_fb = 0;
        memset(&fbs[i], 0, sizeof(fbs[i]));
      }
    }
    for (int i = 0; i < DRM_MAX_OBJECTS; i++) {
      if (buffers[i].owner == client) {
        buffers[i].owner = 0;
        buffers[i].pending_free = 1;
        maybe_free_buffer(&buffers[i]);
      }
    }
    idr_destroy(&client->handles);
    kfree(client);
  }
  h->private_data = 0;
  if (h->node)
    vfs_node_put(h->node);
  h->node = 0;
}

static const struct vfs_file_ops drm_ops = {
    .read = drm_read,
    .poll = drm_poll,
    .release = drm_release,
    .ioctl = drm_ioctl,
};

/* Resolve one page of a mapping. Scatter-gather objects have no single base, so
 * this is the per-page callback rather than the contiguous one. */
static int drm_mmap_page_phys(struct vfs_handle *handle, u64 offset,
                              u64 *out_phys) {
  struct drm_client *client = handle ? handle->private_data : 0;
  struct drm_buffer *buffer = find_mapped_buffer(offset, PAGE_SIZE);
  if (!buffer || buffer->owner != client)
    return -EINVAL;
  u64 base = drm_slot_mmap_offset((int)(buffer - buffers));
  u64 within = offset - base;
  u64 phys = sg_phys_at(&buffer->sgt, within);
  if (!phys)
    return -EINVAL;
  *out_phys = phys;
  return 0;
}

static void drm_mmap_open(struct vfs_node *node, u64 offset, usize length) {
  (void)node;
  struct drm_buffer *buffer = find_mapped_buffer(offset, length);
  if (buffer)
    buffer->map_refs++;
  fb_dev_mapping_open(node);
}

static void drm_mmap_close(struct vfs_node *node, u64 offset, usize length) {
  (void)node;
  struct drm_buffer *buffer = find_mapped_buffer(offset, length);
  if (buffer && buffer->map_refs)
    buffer->map_refs--;
  if (buffer)
    maybe_free_buffer(buffer);
  fb_dev_mapping_close(node);
}

int drm_dev_open(int flags) {
  if (!card_node)
    return -ENXIO;
  struct drm_client *client = kzalloc(sizeof(*client));
  if (!client)
    return -ENOMEM;
  /* GEM handles start at 1: zero is "no handle" in every DRM ABI. */
  idr_init_base(&client->handles, 1);
  client->next_fb_id = 1;
  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);
  if (!h) {
    idr_destroy(&client->handles);
    kfree(client);
    return -ENFILE;
  }
  h->node = vfs_node_get(card_node);
  h->private_data = client;
  h->ops = &drm_ops;
  h->flags = flags;
  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return -EMFILE;
  }
  if (flags & B1NIX_O_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

/* The GEM linear-view window, for the M100 self-test: it asserts that this
 * range's page-table path is shared by every address space rather than taking
 * the claim on trust. Returns 0 when there is no DRM device (no GPU), in which
 * case there is no window and nothing to assert. */
u64 drm_vmap_window_base(void) { return card_node ? DRM_VMAP_BASE : 0; }
u64 drm_vmap_window_size(void) { return DRM_VMAP_WINDOW_TOTAL; }

/* An attribute whose value is a string owned by the registry. */
static isize drm_sysfs_text_show(void *ctx, char *buf, usize cap) {
  const char *text = (const char *)ctx;
  usize n = 0;

  while (text[n] && n + 1 < cap) { buf[n] = text[n]; n++; }
  buf[n] = 0;
  return (isize)n;
}

static void drm_sysfs_text_free(void *ctx) { kfree(ctx); }

/*
 * The sysfs a graphics stack discovers a card through.
 *
 * Nothing that drives a display is told where the device is: libudev enumerates
 * /sys/class/drm, reads each entry's uevent for the device node, and follows
 * `subsystem` and `device` to decide what it found. libdrm separately stats
 * /sys/dev/char/<major>:<minor>/device/drm before it will call a descriptor a
 * primary node. Publish that shape and every compositor built on those two
 * libraries — wlroots, and the ones that are not — finds the card by itself,
 * with no environment variable naming it.
 *
 * The layout mirrors Linux's:
 *
 *   /sys/devices/gpu<n>/                  the device
 *              /drm/card<n>/              its DRM minor
 *                   uevent, dev
 *                   subsystem -> /sys/class/drm
 *                   device    -> /sys/devices/gpu<n>
 *   /sys/class/drm/card<n>   -> the minor
 *   /sys/dev/char/226:<n>    -> the minor
 */
static struct sysfs_dir *drm_sysfs_pci_device_id(const struct drm_pci_ident *id);

/*
 * The PCI slot the minors of this device hang off, spelled the way sysfs
 * spells it.
 *
 * One place, because the directory the attributes are written into and the
 * /sys/class and /sys/dev/char links that point AT it have to agree. They did
 * not: the directory was built from the card's real address while both links
 * were built from a hardcoded "0000:00:02.0", so every device that was not at
 * that address published two dangling links. libdrm resolves a minor through
 * exactly those links, so the node was invisible to Mesa — /dev/dri/renderD128
 * existed, /sys/class/drm/renderD128 existed, and reading its `dev` attribute
 * through the link failed with ENOENT.
 */
static void drm_sysfs_slotname(const struct drm_pci_ident *id, char *buf,
                               usize n) {
	snprintf(buf, n, "%04x:%02x:%02x.%u", 0, id ? id->bus : 0,
	         id ? id->slot : 2, id ? id->func : 0u);
}

/*
 * `uevent`, writable, because that is how a coldplug replay reaches a device.
 *
 * udevd learns about a device either from a netlink announcement it was
 * listening for, or -- for everything that already existed when it started --
 * from `udevadm trigger`, which walks /sys and writes "add" to every uevent
 * file. With the DRM minors' uevent read-only that walk answered "Permission
 * denied" on each card, no announcement was ever made for them, and no rule
 * ran: /run/udev/data/c226:N was never written, the card carried no seat tag,
 * and logind refused to hand it to a compositor. PCI already learned this
 * (kernel/dev/pci.c); the graphics minors had not.
 *
 * The write re-announces the device for real. Accepting it and doing nothing
 * would be exactly the silently-discarded write the sysfs registry's contract
 * forbids, and it would leave the same hole with a file that looks fixed.
 */
struct drm_sysfs_uevent {
	char devpath[96];
	char devname[32];
	char text[128];
	int minor;
};

static isize drm_sysfs_uevent_show(void *ctx, char *buf, usize cap) {
	struct drm_sysfs_uevent *u = (struct drm_sysfs_uevent *)ctx;

	return drm_sysfs_text_show(u ? u->text : "", buf, cap);
}

static isize drm_sysfs_uevent_store(void *ctx, const char *buf, usize len) {
	struct drm_sysfs_uevent *u = (struct drm_sysfs_uevent *)ctx;
	char action[16];
	usize n = 0;

	if (!u || !buf)
		return -EINVAL;
	/* The first word is the action; udev appends a synthetic-event UUID after
	 * it, which nothing here needs. */
	while (n < len && n < sizeof(action) - 1 && buf[n] != ' ' &&
	       buf[n] != '\n' && buf[n] != '\0')
		n++;
	memcpy(action, buf, n);
	action[n] = '\0';

	static const char *const known[] = {"add",    "remove", "change",
	                                    "move",   "online", "offline",
	                                    "bind",   "unbind"};
	int ok = 0;
	for (usize i = 0; i < sizeof(known) / sizeof(known[0]); i++)
		if (strcmp(action, known[i]) == 0) { ok = 1; break; }
	if (!ok)
		return -EINVAL;

	/* DEVTYPE matters here: udev seals an sd_device built from a netlink
	 * announcement, so a rule that asks for drm_minor gets no answer at all
	 * unless the announcement carries it. */
	uevent_post(action, u->devpath, "drm", "drm_minor", u->devname, 226,
	            u->minor);
	return (isize)len;
}

/* Register the minor's uevent file, writable, with the properties Linux puts
 * in it. `slot` is the PCI slot the minor hangs off, which is what makes the
 * DEVPATH the announcement carries the same path udev finds in /sys. */
static void drm_sysfs_reg_uevent(struct sysfs_dir *minor, const char *slot,
                                 const char *nodename, unsigned minor_num) {
	struct drm_sysfs_uevent *u = kzalloc(sizeof(*u));

	if (!u)
		return;
	snprintf(u->devpath, sizeof(u->devpath), "/devices/pci0000:00/%s/drm/%s",
	         slot, nodename);
	snprintf(u->devname, sizeof(u->devname), "dri/%s", nodename);
	snprintf(u->text, sizeof(u->text),
	         "MAJOR=226\nMINOR=%u\nDEVNAME=dri/%s\nDEVTYPE=drm_minor\n",
	         minor_num, nodename);
	u->minor = (int)minor_num;
	if (sysfs_reg_attr(minor, "uevent", 0644, drm_sysfs_uevent_show,
	                   drm_sysfs_uevent_store, u, drm_sysfs_text_free) != 0)
		kfree(u);
}

void drm_sysfs_publish_card(unsigned num) { drm_sysfs_publish_card_id(num, 0); }

void drm_sysfs_publish_card_id(unsigned num, const struct drm_pci_ident *id) {
	char card[16], node[24], minordir[64], *devtext;

	snprintf(card, sizeof(card), "card%u", num);
	snprintf(node, sizeof(node), "226:%u", num);

	/* Under the PCI device, for the same reason the render node is: libdrm
	 * reads a minor's identity from its parent, and a parent with no PCI
	 * attributes leaves the device undescribable and therefore invisible. */
	struct sysfs_dir *dev = drm_sysfs_pci_device_id(id);
	struct sysfs_dir *drm = dev ? sysfs_reg_dir(dev, "drm") : 0;
	struct sysfs_dir *minor = drm ? sysfs_reg_dir(drm, card) : 0;

	if (!minor)
		return;

	/* uevent is where udev reads the device node's name and numbers from -- and
	 * where `udevadm trigger` writes to have the device announced again. A card
	 * with no uevent is enumerated and then discarded as having no node; one
	 * that cannot be written to is never coldplugged at all. */
	{
		char slot[16];

		drm_sysfs_slotname(id, slot, sizeof(slot));
		drm_sysfs_reg_uevent(minor, slot, card, num);
	}
	devtext = kmalloc(16);
	if (devtext) {
		snprintf(devtext, 16, "226:%u\n", num);
		if (sysfs_reg_attr(minor, "dev", 0444, drm_sysfs_text_show, 0, devtext,
		                   drm_sysfs_text_free) != 0)
			kfree(devtext);
	}

	/*
	 * Relative targets, because that is what a sysfs link is.
	 *
	 * One "../" per component between the link and /sys, which for a minor
	 * under /sys/devices/pci0000:00/<slot>/drm/card<N> is five. Six was one
	 * too many: it walks past the mount point, and a resolver that clamps
	 * ".." at the root answers "/class/drm", a path that exists nowhere,
	 * while one that splices by counting is left with a fragment. Linux
	 * writes the exact count, and so does this.
	 *
	 * libudev resolves these by hand rather than with realpath: it counts the
	 * leading "../" and splices the remainder onto the directory the link lives
	 * in. An absolute target survives that as nonsense — "/sys/class/drm" with
	 * "/sys/devices/..." glued to the end — and the device is silently dropped,
	 * which is how a card that was plainly in /sys came back as "Found 0 GPUs".
	 */
	(void)sysfs_reg_link(minor, "subsystem", "../../../../../class/drm");
	(void)sysfs_reg_link(minor, "device", "../..");

	{
		char slot[16];

		drm_sysfs_slotname(id, slot, sizeof(slot));
		snprintf(minordir, sizeof(minordir),
		         "../../devices/pci0000:00/%s/drm/card%u", slot, num);
	}

	struct sysfs_dir *cls = sysfs_reg_dir(sysfs_reg_dir(0, "class"), "drm");
	if (cls)
		(void)sysfs_reg_link(cls, card, minordir);

	struct sysfs_dir *chr =
		sysfs_reg_dir(sysfs_reg_dir(0, "dev"), "char");
	if (chr)
		(void)sysfs_reg_link(chr, node, minordir);
}

/*
 * The PCI device the DRM minors belong to.
 *
 * libdrm does not enumerate devices by walking /dev: drmGetDevices2 starts from
 * the minor's sysfs entry, follows its "device" link, and reads the PCI
 * identity there — vendor, device, revision, and the slot name in uevent. A
 * minor whose parent has none of that is not a device it can describe, so it is
 * dropped, and a caller that asked for the list gets nothing. That is why
 * Chromium reported no render node while answering its version query correctly:
 * it never enumerated ours at all.
 *
 * The values are the passed-through GPU's own (Intel 8086:3e98, slot
 * 0000:00:02.0) — the same ones the guest's PCI config space reports.
 */
/*
 * The sysfs directory for one PCI function, described as itself.
 *
 * This used to be a single cached directory with the Intel part's ids written
 * into it, shared by every DRM minor. With one card that is merely untrue; with
 * two it is fatal, because the graphics stack tells cards apart by the device
 * they hang off. Mesa enumerates EGL devices, resolves each to a DRM node
 * through sysfs, finds both nodes on the same device and initialises whichever
 * it saw first — which is how a compositor driving the Intel GPU ended up
 * asking iris to create a screen on the virtual one.
 *
 * `id` NULL keeps the old behaviour for callers that have no identity to give.
 */
static struct sysfs_dir *drm_sysfs_pci_device_id(const struct drm_pci_ident *id)
{
	char slotname[16];
	char vendor_s[16], device_s[16], subv_s[16], subd_s[16], rev_s[16],
	     class_s[16], uevent_s[192];

	drm_sysfs_slotname(id, slotname, sizeof(slotname));
	/*
	 * With no identity, publish none.
	 *
	 * These fields used to fall back to the passed-through Intel part's ids
	 * (8086:3e98 at 00:02.0) whenever the PCI lookup came up empty. A card
	 * described as a device it is not is worse than a card described as
	 * nothing: the graphics stack tells cards apart by the device they hang
	 * off, which is exactly how a compositor driving one GPU ended up asking
	 * iris for a screen on the other. An absent attribute is a truthful "this
	 * kernel does not know"; a borrowed one is an answer userspace cannot
	 * check. The directory itself stays either way, so the minor beneath it is
	 * still enumerable.
	 */
	vendor_s[0] = device_s[0] = subv_s[0] = subd_s[0] = 0;
	rev_s[0] = class_s[0] = uevent_s[0] = 0;
	if (id) {
		snprintf(vendor_s, sizeof(vendor_s), "0x%04x\n", id->vendor);
		snprintf(device_s, sizeof(device_s), "0x%04x\n", id->device);
		snprintf(subv_s, sizeof(subv_s), "0x%04x\n", id->subsystem_vendor);
		snprintf(subd_s, sizeof(subd_s), "0x%04x\n", id->subsystem_device);
		snprintf(rev_s, sizeof(rev_s), "0x%02x\n", id->revision);
		snprintf(class_s, sizeof(class_s), "0x%06x\n", id->pci_class);
		snprintf(uevent_s, sizeof(uevent_s),
		         "DRIVER=%s\nPCI_CLASS=%X\nPCI_ID=%04X:%04X\n"
		         "PCI_SUBSYS_ID=%04X:%04X\nPCI_SLOT_NAME=%s\n",
		         id->driver ? id->driver : "drm", id->pci_class,
		         id->vendor, id->device, id->subsystem_vendor,
		         id->subsystem_device, slotname);
	}

	struct sysfs_dir *devices = sysfs_reg_dir(0, "devices");
	struct sysfs_dir *root = sysfs_reg_dir(devices, "pci0000:00");
	struct sysfs_dir *dev = sysfs_reg_dir(root, slotname);

	if (!dev)
		return 0;

	const struct { const char *name; const char *value; } attrs[] = {
		{ "vendor",            vendor_s },
		{ "device",            device_s },
		{ "subsystem_vendor",  subv_s },
		{ "subsystem_device",  subd_s },
		{ "revision",          rev_s },
		{ "class",             class_s },
		{ "uevent",            uevent_s },
	};

	for (unsigned i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
		usize len = strlen(attrs[i].value) + 1;
		char *text;

		/* Empty means unknown -- see above. Nothing is published for it. */
		if (attrs[i].value[0] == 0)
			continue;
		text = kmalloc(len);
		if (!text)
			continue;
		memcpy(text, attrs[i].value, len);
		if (sysfs_reg_attr(dev, attrs[i].name, 0444, drm_sysfs_text_show, 0,
		                   text, drm_sysfs_text_free) != 0)
			kfree(text);
	}

	(void)sysfs_reg_link(dev, "subsystem", "../../../bus/pci");

	struct sysfs_dir *bus = sysfs_reg_dir(sysfs_reg_dir(0, "bus"), "pci");
	struct sysfs_dir *busdev = sysfs_reg_dir(bus, "devices");

	if (busdev) {
		char target[80];

		snprintf(target, sizeof(target),
		         "../../../devices/pci0000:00/%s", slotname);
		(void)sysfs_reg_link(busdev, slotname, target);
	}

	return dev;
}



/*
 * The same publication for a render node.
 *
 * A device node in /dev is only half of what a graphics stack looks for: Mesa
 * and Chromium enumerate through sysfs (libudev, or by hand through
 * /sys/dev/char) and take the node's path from the uevent found there. A
 * renderD128 that exists in /dev but nowhere in /sys is therefore invisible to
 * them — which is what "Failed to initialize drm render node handle" meant even
 * after the device node itself was created.
 *
 * It hangs off the same gpu directory as its card: they are two minors of one
 * device, and that is what the "device" link says.
 */
void drm_sysfs_publish_render(unsigned gpu_num, unsigned minor_num)
{ drm_sysfs_publish_render_id(gpu_num, minor_num, 0); }

void drm_sysfs_publish_render_id(unsigned gpu_num, unsigned minor_num,
                                 const struct drm_pci_ident *id) {
	char name[16], node[24], minordir[72], *devtext;

	snprintf(name, sizeof(name), "renderD%u", minor_num);
	snprintf(node, sizeof(node), "226:%u", minor_num);

	(void)gpu_num;
	struct sysfs_dir *dev = drm_sysfs_pci_device_id(id);
	struct sysfs_dir *drm = dev ? sysfs_reg_dir(dev, "drm") : 0;
	struct sysfs_dir *minor = drm ? sysfs_reg_dir(drm, name) : 0;

	if (!minor)
		return;

	{
		char slot[16];

		drm_sysfs_slotname(id, slot, sizeof(slot));
		drm_sysfs_reg_uevent(minor, slot, name, minor_num);
	}
	devtext = kmalloc(16);
	if (devtext) {
		snprintf(devtext, 16, "226:%u\n", minor_num);
		if (sysfs_reg_attr(minor, "dev", 0444, drm_sysfs_text_show, 0, devtext,
		                   drm_sysfs_text_free) != 0)
			kfree(devtext);
	}

	/* Up to the PCI device, which is what drmGetDevices2 reads the identity
	 * from; and out to the class, which is how it finds minors at all. */
	(void)sysfs_reg_link(minor, "subsystem", "../../../../../class/drm");
	(void)sysfs_reg_link(minor, "device", "../..");

	{
		char slot[16];

		drm_sysfs_slotname(id, slot, sizeof(slot));
		snprintf(minordir, sizeof(minordir),
		         "../../devices/pci0000:00/%s/drm/renderD%u", slot, minor_num);
	}

	struct sysfs_dir *cls = sysfs_reg_dir(sysfs_reg_dir(0, "class"), "drm");
	if (cls)
		(void)sysfs_reg_link(cls, name, minordir);

	struct sysfs_dir *chr = sysfs_reg_dir(sysfs_reg_dir(0, "dev"), "char");
	if (chr)
		(void)sysfs_reg_link(chr, node, minordir);
}

void drm_dev_init(void) {
  if (!virtio_gpu_ready())
    return;
  struct vfs_node *dir = vfs_add_node("/dev/dri", VFS_DIRECTORY, 0, 0, 0);
  if (dir && !IS_ERR(dir))
    vfs_node_put(dir);
  card_node = vfs_add_node("/dev/dri/card0", VFS_DEVICE, 0, 0, 0);
  if (!card_node || IS_ERR(card_node)) {
    card_node = 0;
    console_write("drm: failed to register /dev/dri/card0\n");
    return;
  }
  /* Create the object-window's page-table path NOW, while we are still running
   * on the kernel address space.
   *
   * paging_create_address_space() copies the kernel-half PML4 entries (256-511)
   * *by value* into every new address space. Anything below that level is
   * therefore shared automatically — but a PML4 entry that does not exist yet
   * when a process is created never appears in that process. Buffer objects are
   * allocated from an ioctl, i.e. on a user address space, so the window's PML4
   * entry would be created in exactly one process and be missing everywhere
   * else: a GEM mapping would then fault in one process and work in another.
   *
   * Reserve the path explicitly rather than mapping a page and unmapping it
   * again, which only worked because unmap leaves the levels above the leaf in
   * place — an invariant nothing stated and nothing checked. The M100 self-test
   * now asserts the result: a freshly created address space must carry the same
   * PML4 entry the kernel table holds for this window. */
  paging_reserve_kernel_path(DRM_VMAP_BASE, DRM_VMAP_WINDOW_TOTAL);

  card_node->inode->mode = 0600;
  /* The real DRM device numbers, because programs identify a card by them
   * rather than by its name: seatd decides whether it may open a device by
   * major, and refuses one it cannot classify. Carried in inode->rdev, which is
   * what fstat reports as st_rdev. */
  card_node->inode->rdev = ((u64)226 << 8) | 0;
  /* Deliberately not published to /sys/class/drm.
   *
   * This node is b1nix's own small DRM device, kept for the tests written
   * against it. The imported core drives the same hardware through card1, so
   * advertising both would offer a compositor two devices for one GPU — and the
   * one it must not choose is this one. Discovery sees the real cards only. */
  card_node->inode->mmap_handle_page_phys_cb = drm_mmap_page_phys;
  card_node->inode->mmap_range_open_cb = drm_mmap_open;
  card_node->inode->mmap_range_close_cb = drm_mmap_close;
  console_write("drm: /dev/dri/card0 ready (scatter-gather GEM)\n");
}
