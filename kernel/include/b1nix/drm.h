#ifndef B1NIX_DRM_H
#define B1NIX_DRM_H

#include <b1nix/types.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr, type)                                                     \
  (((3U) << 30) | ((u32)sizeof(type) << 16) | ((u32)DRM_IOCTL_BASE << 8) |     \
   (nr))

#define DRM_MODE_CONNECTED 1
#define DRM_MODE_CONNECTOR_VIRTUAL 15
#define DRM_MODE_PAGE_FLIP_EVENT 1

struct drm_mode_modeinfo {
  u32 clock;
  u16 hdisplay, hsync_start, hsync_end, htotal, hskew;
  u16 vdisplay, vsync_start, vsync_end, vtotal, vscan;
  u32 vrefresh;
  u32 flags;
  u32 type;
  char name[32];
};

struct drm_mode_card_res {
  u64 fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
  u32 count_fbs, count_crtcs, count_connectors, count_encoders;
  u32 min_width, max_width, min_height, max_height;
};

struct drm_mode_get_connector {
  u64 encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
  u32 count_modes, count_props, count_encoders;
  u32 encoder_id, connector_id, connector_type, connector_type_id;
  u32 connection, mm_width, mm_height, subpixel, pad;
};

struct drm_mode_create_dumb {
  u32 height, width, bpp, flags;
  u32 handle, pitch;
  u64 size;
};

struct drm_mode_map_dumb {
  u32 handle, pad;
  u64 offset;
};

struct drm_mode_destroy_dumb {
  u32 handle;
};

struct drm_mode_fb_cmd {
  u32 fb_id, width, height, pitch, bpp, depth, handle;
};

struct drm_mode_crtc {
  u64 set_connectors_ptr;
  u32 count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
  struct drm_mode_modeinfo mode;
};

struct drm_mode_crtc_page_flip {
  u32 crtc_id, fb_id, flags, reserved;
  u64 user_data;
};

struct drm_event {
  u32 type;
  u32 length;
};

struct drm_event_vblank {
  struct drm_event base;
  u64 user_data;
  u32 tv_sec, tv_usec, sequence, crtc_id;
};

#define DRM_EVENT_FLIP_COMPLETE 0x02

#define DRM_IOCTL_MODE_GETRESOURCES                                            \
  DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_SETCRTC DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETCONNECTOR                                            \
  DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB DRM_IOWR(0xAF, u32)
#define DRM_IOCTL_MODE_PAGE_FLIP                                               \
  DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_CREATE_DUMB                                             \
  DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB                                            \
  DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)

/* M100: report how a GEM object is physically backed. b1nix-specific (there is
 * no Linux ioctl for this) and read-only, so it cannot change driver state; its
 * purpose is to let a test assert that a buffer really is scatter-gather backed
 * rather than one contiguous allocation that happens to work. */
struct drm_b1nix_gem_info {
  u32 handle;
  u32 nents;      /* scatter-gather entries describing the object */
  u32 npages;     /* pages the object occupies */
  u32 contiguous; /* 1 when those pages form a single physical run */
  u64 size;
};
#define DRM_IOCTL_B1NIX_GEM_INFO DRM_IOWR(0xC0, struct drm_b1nix_gem_info)

void drm_dev_init(void);
int drm_dev_open(int flags);

/* M101t: /dev/dri/card1, the imported DRM core's own node. Registered only when
 * a device came up on that core, so `present` is the honest question to ask
 * before routing an open at it. */
void drm_card1_init(void);
int drm_card1_open(int flags);
int drm_card1_present(void);

/* M100: the GEM linear-view kernel window (0 when no DRM device exists). The
 * self-test uses these to check the window's page-table path really is shared
 * by every address space. */
u64 drm_vmap_window_base(void);
u64 drm_vmap_window_size(void);

#endif
