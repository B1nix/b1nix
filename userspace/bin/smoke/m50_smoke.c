#include <b1nix/drm.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void mark(const char *s) { write(1, s, strlen(s)); }

static int create_buffer(int fd, const struct drm_mode_modeinfo *mode,
                         struct drm_mode_create_dumb *d,
                         struct drm_mode_fb_cmd *fb, uint32_t **pixels) {
  memset(d, 0, sizeof(*d));
  d->width = mode->hdisplay;
  d->height = mode->vdisplay;
  d->bpp = 32;
  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, d) < 0)
    return -1;
  struct drm_mode_map_dumb map = {.handle = d->handle};
  if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
    return -1;
  *pixels = mmap(0, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 (long)map.offset);
  if (*pixels == MAP_FAILED)
    return -1;
  memset(fb, 0, sizeof(*fb));
  fb->width = d->width;
  fb->height = d->height;
  fb->pitch = d->pitch;
  fb->bpp = 32;
  fb->depth = 24;
  fb->handle = d->handle;
  return ioctl(fd, DRM_IOCTL_MODE_ADDFB, fb);
}

int main(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    mark("M50-DRM: fail card0\n");
    return 1;
  }
  mark("M50-DRM: ok card0\n");

  uint32_t crtc = 0, connector = 0;
  struct drm_mode_card_res r = {
      .crtc_id_ptr = (uint64_t)(uintptr_t)&crtc,
      .connector_id_ptr = (uint64_t)(uintptr_t)&connector,
      .count_crtcs = 1,
      .count_connectors = 1,
  };
  struct drm_mode_modeinfo mode;
  struct drm_mode_get_connector c = {
      .modes_ptr = (uint64_t)(uintptr_t)&mode,
      .count_modes = 1,
      .connector_id = 1,
  };
  if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0 ||
      ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || crtc != 1 ||
      connector != 1 || c.connection != DRM_MODE_CONNECTED ||
      !mode.hdisplay || !mode.vdisplay) {
    mark("M50-DRM: fail mode\n");
    return 1;
  }
  mark("M50-DRM: ok mode\n");

  struct drm_mode_create_dumb d[2];
  struct drm_mode_fb_cmd fb[2];
  uint32_t *pixels[2];
  if (create_buffer(fd, &mode, &d[0], &fb[0], &pixels[0]) < 0 ||
      create_buffer(fd, &mode, &d[1], &fb[1], &pixels[1]) < 0 ||
      d[0].handle == d[1].handle || fb[0].fb_id == fb[1].fb_id) {
    mark("M50-DRM: fail multi-buffer\n");
    return 1;
  }
  pixels[0][0] = 0x00112233;
  pixels[1][0] = 0x00445566;
  mark("M50-DRM: ok multi-buffer\n");

  uint32_t connector_id = connector;
  struct drm_mode_crtc set = {
      .set_connectors_ptr = (uint64_t)(uintptr_t)&connector_id,
      .count_connectors = 1,
      .crtc_id = crtc,
      .fb_id = fb[0].fb_id,
      .mode_valid = 1,
      .mode = mode,
  };
  if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
    mark("M50-DRM: fail setcrtc\n");
    return 1;
  }
  mark("M50-DRM: ok setcrtc\n");

  struct drm_mode_crtc_page_flip flip = {
      .crtc_id = crtc,
      .fb_id = fb[1].fb_id,
      .flags = DRM_MODE_PAGE_FLIP_EVENT,
      .user_data = 0x50,
  };
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  struct drm_event_vblank event;
  if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0 ||
      poll(&pfd, 1, 1000) != 1 || !(pfd.revents & POLLIN) ||
      read(fd, &event, sizeof(event)) != (int)sizeof(event) ||
      event.base.type != DRM_EVENT_FLIP_COMPLETE ||
      event.user_data != 0x50 || event.crtc_id != crtc) {
    mark("M50-DRM: fail flip-event\n");
    return 1;
  }
  mark("M50-DRM: ok flip-event\n");

  if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb[0].fb_id) < 0 ||
      ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb[1].fb_id) < 0) {
    mark("M50-DRM: fail rmfb\n");
    return 1;
  }
  mark("M50-DRM: ok rmfb\n");

  munmap(pixels[0], d[0].size);
  struct drm_mode_destroy_dumb destroy = {.handle = d[0].handle};
  if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0) {
    mark("M50-DRM: fail destroy\n");
    return 1;
  }

  /* Leave buffer 2 mapped while closing: release drops its handle/FB state,
   * while the VMA keeps the physical pages alive until munmap. */
  close(fd);
  if (pixels[1][0] != 0x00445566) {
    mark("M50-DRM: fail close-map\n");
    return 1;
  }
  munmap(pixels[1], d[1].size);

  fd = open("/dev/dri/card0", O_RDWR);
  struct drm_mode_create_dumb probe = {
      .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32};
  if (fd < 0 || ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &probe) < 0) {
    mark("M50-DRM: fail cleanup\n");
    return 1;
  }
  destroy.handle = probe.handle;
  if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0) {
    mark("M50-DRM: fail cleanup\n");
    return 1;
  }
  close(fd);
  mark("M50-DRM: ok cleanup\n");
  mark("M50-DRM: done\n");
  return 0;
}
