#ifndef B1NIX_U_DRM_H
#define B1NIX_U_DRM_H

#include <stdint.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr, type)                                                     \
  (((3U) << 30) | ((uint32_t)sizeof(type) << 16) |                             \
   ((uint32_t)DRM_IOCTL_BASE << 8) | (nr))

#define DRM_MODE_CONNECTED 1
#define DRM_MODE_CONNECTOR_VIRTUAL 15

struct drm_mode_modeinfo {
  uint32_t clock;
  uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
  uint32_t vrefresh;
  uint32_t flags;
  uint32_t type;
  char name[32];
};

struct drm_mode_card_res {
  uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
  uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
  uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_get_connector {
  uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
  uint32_t count_modes, count_props, count_encoders;
  uint32_t encoder_id, connector_id, connector_type, connector_type_id;
  uint32_t connection, mm_width, mm_height, subpixel, pad;
};

struct drm_mode_create_dumb {
  uint32_t height, width, bpp, flags, handle, pitch;
  uint64_t size;
};
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb { uint32_t handle; };
struct drm_mode_fb_cmd {
  uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};
struct drm_mode_crtc {
  uint64_t set_connectors_ptr;
  uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
  struct drm_mode_modeinfo mode;
};
struct drm_mode_crtc_page_flip {
  uint32_t crtc_id, fb_id, flags, reserved;
  uint64_t user_data;
};
struct drm_event { uint32_t type, length; };
struct drm_event_vblank {
  struct drm_event base;
  uint64_t user_data;
  uint32_t tv_sec, tv_usec, sequence, crtc_id;
};

#define DRM_MODE_PAGE_FLIP_EVENT 1
#define DRM_EVENT_FLIP_COMPLETE 0x02

#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_SETCRTC DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB DRM_IOWR(0xAF, uint32_t)
#define DRM_IOCTL_MODE_PAGE_FLIP DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_CREATE_DUMB DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)

#endif
