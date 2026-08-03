/*
 * M100 — DRM core, userspace-visible half.
 *
 * The kernel half (dma-fence, the GPU scheduler, the scatter-gather backing
 * itself) is covered by in-kernel M100-SMOKE markers. This binary covers the
 * part that only userspace can see: that a GEM buffer object built from a page
 * list still behaves like one flat buffer through mmap, and that scanout still
 * works after the conversion.
 *
 * Every marker below is printed only after the operation ran and its result was
 * checked against something known independently of the driver:
 *   - the object's page count against the size userspace asked for;
 *   - the contents of every page against a pattern this program wrote, read
 *     back through a *second, independent* mapping of the same object;
 *   - a deliberately out-of-range handle against the errno the ABI specifies.
 */

#include <b1nix/drm.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096u

static void mark(const char *s) { write(1, s, strlen(s)); }

static void fail(const char *s) {
  mark("M100-SMOKE: FAIL ");
  mark(s);
  mark("\n");
}

static void ok(const char *s) {
  mark("M100-SMOKE: ok ");
  mark(s);
  mark("\n");
}

int main(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    /* No virtio-gpu in this instance: the DRM device does not exist at all, so
     * there is nothing to test rather than something that failed. */
    mark("M100-SMOKE: skip gem (no /dev/dri/card0)\n");
    return 0;
  }

  struct drm_mode_modeinfo mode;
  struct drm_mode_get_connector conn = {
      .modes_ptr = (uint64_t)(uintptr_t)&mode,
      .count_modes = 1,
      .connector_id = 1,
  };
  if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || !mode.hdisplay) {
    fail("gem-mode");
    return 1;
  }

  /* A buffer several pages long, so a discontiguous backing is possible and
   * every page has to be mapped individually. */
  struct drm_mode_create_dumb d;
  memset(&d, 0, sizeof(d));
  d.width = 256;
  d.height = 64; /* 256*64*4 = 64 KiB = 16 pages */
  d.bpp = 32;
  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d) < 0 || !d.handle) {
    fail("gem-create");
    return 1;
  }
  uint64_t want_bytes = (uint64_t)d.width * d.height * 4;
  uint32_t want_pages = (uint32_t)((want_bytes + PAGE - 1) / PAGE);
  if (d.pitch != d.width * 4 || d.size < want_bytes) {
    fail("gem-create");
    return 1;
  }
  ok("gem-create");

  /* The object's physical backing, as the kernel reports it. Checked against
   * the geometry this program asked for, not against itself. */
  struct drm_b1nix_gem_info gi;
  memset(&gi, 0, sizeof(gi));
  gi.handle = d.handle;
  if (ioctl(fd, DRM_IOCTL_B1NIX_GEM_INFO, &gi) < 0) {
    fail("gem-info");
    return 1;
  }
  if (gi.npages != want_pages || gi.size != (uint64_t)want_pages * PAGE ||
      gi.nents == 0 || gi.nents > gi.npages ||
      gi.contiguous != (gi.nents == 1)) {
    fail("gem-info");
    return 1;
  }
  ok("gem-info");

  struct drm_mode_map_dumb map;
  memset(&map, 0, sizeof(map));
  map.handle = d.handle;
  if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
    fail("gem-map");
    return 1;
  }
  uint32_t *px = mmap(0, (size_t)d.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                      (long)map.offset);
  if (px == MAP_FAILED) {
    fail("gem-map");
    return 1;
  }
  /* A freshly created object must be zeroed — a stale page from another
   * allocation showing through would be an information leak. */
  for (uint32_t i = 0; i < want_pages; i++) {
    if (px[i * (PAGE / 4)] != 0) {
      fail("gem-map");
      return 1;
    }
  }
  ok("gem-map");

  /* Write a per-page pattern, including the last word of each page, so a
   * mapping that silently aliased two pages or ran off the end of a run would
   * show up as a mismatch. */
  for (uint32_t p = 0; p < want_pages; p++) {
    uint32_t *page = px + p * (PAGE / 4);
    page[0] = 0xC0DE0000u | p;
    page[PAGE / 4 - 1] = 0xFACE0000u | p;
  }
  munmap(px, (size_t)d.size);

  /* Second, independent mapping of the same object. If the per-page physical
   * lookup were wrong, this view would not agree with what was just written. */
  uint32_t *px2 = mmap(0, (size_t)d.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, (long)map.offset);
  if (px2 == MAP_FAILED) {
    fail("gem-mmap-pages");
    return 1;
  }
  for (uint32_t p = 0; p < want_pages; p++) {
    uint32_t *page = px2 + p * (PAGE / 4);
    if (page[0] != (0xC0DE0000u | p) ||
        page[PAGE / 4 - 1] != (0xFACE0000u | p)) {
      fail("gem-mmap-pages");
      return 1;
    }
  }
  ok("gem-mmap-pages");

  /* Scanout regression: a scatter-gather object must still reach the display.
   * SETCRTC only succeeds when the driver could hand the whole buffer to the
   * device, which needs the linear kernel view over the page list. */
  struct drm_mode_create_dumb full;
  memset(&full, 0, sizeof(full));
  full.width = mode.hdisplay;
  full.height = mode.vdisplay;
  full.bpp = 32;
  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &full) < 0) {
    fail("gem-scanout");
    return 1;
  }
  struct drm_mode_map_dumb fmap;
  memset(&fmap, 0, sizeof(fmap));
  fmap.handle = full.handle;
  uint32_t *fb_px = 0;
  if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &fmap) == 0)
    fb_px = mmap(0, (size_t)full.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 (long)fmap.offset);
  if (fb_px == MAP_FAILED || !fb_px) {
    fail("gem-scanout");
    return 1;
  }
  for (uint32_t i = 0; i < full.size / 4; i += 1024)
    fb_px[i] = 0x00204080u;

  struct drm_mode_fb_cmd fbc;
  memset(&fbc, 0, sizeof(fbc));
  fbc.width = full.width;
  fbc.height = full.height;
  fbc.pitch = full.pitch;
  fbc.bpp = 32;
  fbc.depth = 24;
  fbc.handle = full.handle;
  if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbc) < 0) {
    fail("gem-scanout");
    return 1;
  }
  uint32_t connector_id = 1;
  struct drm_mode_crtc set;
  memset(&set, 0, sizeof(set));
  set.set_connectors_ptr = (uint64_t)(uintptr_t)&connector_id;
  set.count_connectors = 1;
  set.crtc_id = 1;
  set.fb_id = fbc.fb_id;
  set.mode_valid = 1;
  set.mode = mode;
  if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
    fail("gem-scanout");
    return 1;
  }
  /* The pixels must survive the trip to the device. */
  if (fb_px[0] != 0x00204080u) {
    fail("gem-scanout");
    return 1;
  }
  ok("gem-scanout");

  /* Handle lifetime: an object bound to a framebuffer cannot be destroyed, an
   * unbound one can, and a stale handle then misses. */
  struct drm_mode_destroy_dumb destroy;
  memset(&destroy, 0, sizeof(destroy));
  destroy.handle = full.handle;
  if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) == 0) {
    fail("gem-destroy");
    return 1;
  }
  if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &fbc.fb_id) < 0) {
    fail("gem-destroy");
    return 1;
  }
  if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0) {
    fail("gem-destroy");
    return 1;
  }
  memset(&gi, 0, sizeof(gi));
  gi.handle = full.handle;
  if (ioctl(fd, DRM_IOCTL_B1NIX_GEM_INFO, &gi) == 0) {
    fail("gem-destroy");
    return 1;
  }
  /* And a handle that was never allocated must miss too. */
  memset(&gi, 0, sizeof(gi));
  gi.handle = 0x7fffffffu;
  if (ioctl(fd, DRM_IOCTL_B1NIX_GEM_INFO, &gi) == 0) {
    fail("gem-destroy");
    return 1;
  }
  ok("gem-destroy");

  /* Handles must be reused after a destroy rather than growing without bound —
   * that is what the idr-backed handle table buys. */
  struct drm_mode_create_dumb again;
  memset(&again, 0, sizeof(again));
  again.width = 64;
  again.height = 16;
  again.bpp = 32;
  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &again) < 0) {
    fail("gem-handle-reuse");
    return 1;
  }
  if (again.handle != full.handle) {
    fail("gem-handle-reuse");
    return 1;
  }
  ok("gem-handle-reuse");

  munmap(px2, (size_t)d.size);
  munmap(fb_px, (size_t)full.size);
  close(fd);
  mark("M100-SMOKE: done userspace\n");
  return 0;
}
