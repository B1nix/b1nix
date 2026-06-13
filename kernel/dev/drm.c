#include <b1nix/console.h>
#include <b1nix/drm.h>
#include <b1nix/errno.h>
#include <b1nix/fb.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <b1nix/virtio_gpu.h>
#include <string.h>

#define DRM_CRTC_ID 1
#define DRM_CONNECTOR_ID 1
#define DRM_ENCODER_ID 1
#define DRM_MAX_OBJECTS 8
#define DRM_MAP_STRIDE (64ULL * 1024 * 1024)

struct drm_client {
  u32 next_handle;
  u32 next_fb_id;
  u32 sequence;
  int event_pending;
  struct drm_event_vblank event;
};

struct drm_buffer {
  struct drm_client *owner;
  u64 phys;
  u32 *virt;
  usize size;
  u32 width, height, pitch, handle;
  u32 map_refs, present_refs;
  int pending_free;
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

static void drm_mode(struct drm_mode_modeinfo *mode) {
  u32 width = 0, height = 0;
  virtio_gpu_get_mode(&width, &height);
  memset(mode, 0, sizeof(*mode));
  mode->hdisplay = (u16)width;
  mode->vdisplay = (u16)height;
  mode->vrefresh = 60;
  strcpy(mode->name, "virtio");
}

static struct drm_buffer *find_buffer(struct drm_client *client, u32 handle) {
  for (int i = 0; i < DRM_MAX_OBJECTS; i++)
    if (buffers[i].owner == client && buffers[i].handle == handle)
      return &buffers[i];
  return 0;
}

static struct drm_buffer *find_mapped_buffer(u64 offset, usize length) {
  for (int i = 0; i < DRM_MAX_OBJECTS; i++) {
    u64 base = (u64)(i + 1) * DRM_MAP_STRIDE;
    if (buffers[i].virt && offset >= base &&
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

static void free_buffer(struct drm_buffer *buffer) {
  u64 phys = buffer->phys;
  usize frames = buffer->size / PAGE_SIZE;
  memset(buffer, 0, sizeof(*buffer));
  for (usize i = 0; i < frames; i++)
    pmm_free_frame(phys + i * PAGE_SIZE);
}

static void maybe_free_buffer(struct drm_buffer *buffer) {
  if (buffer->pending_free && !buffer->map_refs && !buffer->present_refs)
    free_buffer(buffer);
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

static int drm_ioctl(struct vfs_handle *h, u64 request, void *arg) {
  struct drm_client *client = h->private_data;
  if (!client || !arg)
    return -EFAULT;

  switch ((u32)request) {
  case DRM_IOCTL_MODE_GETRESOURCES: {
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
  case DRM_IOCTL_MODE_GETCONNECTOR: {
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
  case DRM_IOCTL_MODE_CREATE_DUMB: {
    struct drm_mode_create_dumb d;
    if (syscall_copyin(&d, arg, sizeof(d)) < 0)
      return -EFAULT;
    if (!d.width || !d.height || d.bpp != 32 ||
        d.width > 65535 || d.height > 65535)
      return -EINVAL;
    u64 bytes = (u64)d.width * d.height * 4;
    if (bytes > DRM_MAP_STRIDE)
      return -ENOMEM;
    int slot = -1;
    for (int i = 0; i < DRM_MAX_OBJECTS; i++)
      if (!buffers[i].virt) {
        slot = i;
        break;
      }
    if (slot < 0)
      return -ENOSPC;
    usize frames = ((usize)bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 phys = pmm_alloc_frames(frames);
    if (!phys)
      return -ENOMEM;
    struct drm_buffer *buffer = &buffers[slot];
    buffer->owner = client;
    buffer->phys = phys;
    buffer->virt = (u32 *)(usize)(phys + vmm_direct_map_base());
    buffer->size = frames * PAGE_SIZE;
    buffer->width = d.width;
    buffer->height = d.height;
    buffer->pitch = d.width * 4;
    buffer->handle = client->next_handle++;
    memset(buffer->virt, 0, buffer->size);
    d.handle = buffer->handle;
    d.pitch = buffer->pitch;
    d.size = buffer->size;
    if (syscall_copyout(arg, &d, sizeof(d)) < 0) {
      buffer->owner = 0;
      buffer->pending_free = 1;
      maybe_free_buffer(buffer);
      return -EFAULT;
    }
    return 0;
  }
  case DRM_IOCTL_MODE_MAP_DUMB: {
    struct drm_mode_map_dumb d;
    if (syscall_copyin(&d, arg, sizeof(d)) < 0)
      return -EFAULT;
    struct drm_buffer *buffer = find_buffer(client, d.handle);
    if (!buffer)
      return -EINVAL;
    d.offset = (u64)(buffer - buffers + 1) * DRM_MAP_STRIDE;
    return syscall_copyout(arg, &d, sizeof(d)) < 0 ? -EFAULT : 0;
  }
  case DRM_IOCTL_MODE_ADDFB: {
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
  case DRM_IOCTL_MODE_RMFB: {
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
  case DRM_IOCTL_MODE_SETCRTC: {
    struct drm_mode_crtc c;
    if (syscall_copyin(&c, arg, sizeof(c)) < 0 || c.crtc_id != DRM_CRTC_ID)
      return -EINVAL;
    struct drm_fb *fb = find_fb(client, c.fb_id);
    if (!fb || (c.mode_valid &&
                (c.mode.hdisplay != fb->buffer->width ||
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
  case DRM_IOCTL_MODE_PAGE_FLIP: {
    struct drm_mode_crtc_page_flip f;
    if (syscall_copyin(&f, arg, sizeof(f)) < 0 ||
        f.crtc_id != DRM_CRTC_ID ||
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
  case DRM_IOCTL_MODE_DESTROY_DUMB: {
    struct drm_mode_destroy_dumb d;
    if (syscall_copyin(&d, arg, sizeof(d)) < 0)
      return -EFAULT;
    struct drm_buffer *buffer = find_buffer(client, d.handle);
    if (!buffer)
      return -EINVAL;
    if (buffer_has_fb(buffer))
      return -EBUSY;
    buffer->owner = 0;
    buffer->pending_free = 1;
    maybe_free_buffer(buffer);
    return 0;
  }
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

static int drm_mmap_phys(struct vfs_handle *handle, u64 offset, usize length,
                         u64 *out_phys) {
  struct drm_client *client = handle ? handle->private_data : 0;
  struct drm_buffer *buffer = find_mapped_buffer(offset, length);
  if (!buffer || buffer->owner != client)
    return -EINVAL;
  u64 base = (u64)(buffer - buffers + 1) * DRM_MAP_STRIDE;
  *out_phys = buffer->phys + offset - base;
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
  client->next_handle = 1;
  client->next_fb_id = 1;
  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);
  if (!h) {
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
  card_node->inode->mode = 0600;
  card_node->inode->mmap_handle_phys_cb = drm_mmap_phys;
  card_node->inode->mmap_range_open_cb = drm_mmap_open;
  card_node->inode->mmap_range_close_cb = drm_mmap_close;
  console_write("drm: /dev/dri/card0 ready\n");
}
