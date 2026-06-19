/* b1nix EGL 1.4 software implementation: a thin shim binding the EGL windowing
 * API to TinyGL software rendering and presenting through a b1gui (Wayland)
 * window. One display, one config, one context (TinyGL keeps a single global GL
 * context). eglSwapBuffers blits TinyGL's framebuffer into the live wl_shm
 * buffer and commits it to displayd.
 *
 * No GPU path here — eglB1nixAccelerated() always reports software. The VirGL
 * accelerated backend (Phase C) plugs in at swap time without changing this API. */
#include <EGL/egl.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <zbuffer.h>

#include <b1nix/gui.h>

/* ZB_MODE_RGBA framebuffer packs pixels as 0x00RRGGBB — same channel order as
 * the ARGB32 wl_shm buffer; we only need to add opaque alpha on present. */

struct b1egl_surface {
  struct b1gui_window *win;
  ZBuffer *zb;
  int width, height;
};

static int g_inited;
static EGLint g_error = EGL_SUCCESS;
static struct b1egl_surface *g_current; /* TinyGL has a single global context */

static EGLDisplay kDisplay = (EGLDisplay)1;
static EGLConfig kConfig = (EGLConfig)1;
static EGLContext kContext = (EGLContext)1;

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  return kDisplay;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  if (dpy != kDisplay) {
    g_error = EGL_BAD_DISPLAY;
    return EGL_FALSE;
  }
  g_inited = 1;
  if (major)
    *major = 1;
  if (minor)
    *minor = 4;
  g_error = EGL_SUCCESS;
  return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
  (void)dpy;
  g_inited = 0;
  return EGL_TRUE;
}

const char *eglQueryString(EGLDisplay dpy, EGLint name) {
  (void)dpy;
  switch (name) {
  case EGL_VENDOR:
    return "b1nix";
  case EGL_VERSION:
    return "1.4 (TinyGL software)";
  default:
    return "";
  }
}

EGLBoolean eglBindAPI(EGLenum api) {
  (void)api;
  return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size,
                           EGLint *num_config) {
  (void)dpy;
  (void)attrib_list;
  if (configs && config_size > 0)
    configs[0] = kConfig;
  if (num_config)
    *num_config = (config_size > 0) ? 1 : 0;
  return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute,
                              EGLint *value) {
  (void)dpy;
  (void)config;
  if (!value)
    return EGL_FALSE;
  switch (attribute) {
  case EGL_RED_SIZE:
  case EGL_GREEN_SIZE:
  case EGL_BLUE_SIZE:
  case EGL_ALPHA_SIZE:
    *value = 8;
    break;
  case EGL_DEPTH_SIZE:
    *value = 16;
    break;
  default:
    *value = 0;
    break;
  }
  return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context,
                            const EGLint *attrib_list) {
  (void)dpy;
  (void)config;
  (void)share_context;
  (void)attrib_list;
  return kContext;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  (void)ctx;
  return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win,
                                  const EGLint *attrib_list) {
  (void)dpy;
  (void)config;
  (void)attrib_list;
  struct b1gui_window *w = (struct b1gui_window *)win;
  if (!w || !w->pixels) {
    g_error = EGL_BAD_SURFACE;
    return EGL_NO_SURFACE;
  }
  struct b1egl_surface *s = calloc(1, sizeof(*s));
  if (!s) {
    g_error = EGL_BAD_SURFACE;
    return EGL_NO_SURFACE;
  }
  s->win = w;
  s->width = (int)w->width;
  s->height = (int)w->height;
  s->zb = ZB_open(s->width, s->height, ZB_MODE_RGBA, NULL);
  if (!s->zb) {
    free(s);
    g_error = EGL_BAD_SURFACE;
    return EGL_NO_SURFACE;
  }
  return (EGLSurface)s;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  struct b1egl_surface *s = (struct b1egl_surface *)surface;
  if (!s)
    return EGL_FALSE;
  if (g_current == s) {
    glClose();
    g_current = NULL;
  }
  if (s->zb)
    ZB_close(s->zb);
  free(s);
  return EGL_TRUE;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                           EGLint *value) {
  (void)dpy;
  struct b1egl_surface *s = (struct b1egl_surface *)surface;
  if (!s || !value)
    return EGL_FALSE;
  if (attribute == EGL_WIDTH)
    *value = s->width;
  else if (attribute == EGL_HEIGHT)
    *value = s->height;
  else
    *value = 0;
  return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                          EGLContext ctx) {
  (void)dpy;
  (void)read;
  (void)ctx;
  struct b1egl_surface *s = (struct b1egl_surface *)draw;
  if (s == g_current)
    return EGL_TRUE;
  if (g_current)
    glClose();
  g_current = s;
  if (s)
    glInit(s->zb); /* binds the global TinyGL context to this surface's ZBuffer */
  return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  struct b1egl_surface *s = (struct b1egl_surface *)surface;
  if (!s || !s->win || !s->win->pixels || !s->zb) {
    g_error = EGL_BAD_SURFACE;
    return EGL_FALSE;
  }
  const PIXEL *src = s->zb->pbuf;
  uint32_t *dst = s->win->pixels;
  uint32_t n = (uint32_t)s->width * (uint32_t)s->height;
  for (uint32_t i = 0; i < n; i++)
    dst[i] = 0xff000000u | ((uint32_t)src[i] & 0x00ffffffu); /* opaque ARGB32 */

  if (b1gui_present(s->win, 0, 0, (uint32_t)s->width, (uint32_t)s->height) < 0) {
    g_error = EGL_BAD_SURFACE;
    return EGL_FALSE;
  }
  /* Wait for displayd to composite this frame so the swap is synchronous. */
  struct b1gui_event ev;
  for (int i = 0; i < 200; i++)
    if (b1gui_next_event(s->win, &ev, 1000) == 1 && ev.type == B1GUI_EV_FRAME)
      break;
  return EGL_TRUE;
}

EGLint eglGetError(void) {
  EGLint e = g_error;
  g_error = EGL_SUCCESS;
  return e;
}

int eglB1nixAccelerated(EGLDisplay dpy) {
  (void)dpy;
  return 0; /* software path; VirGL backend overrides in Phase C */
}

/* The TinyGL window-only backend has no off-screen pbuffer path; the
 * OSMesa-backed b1egl_mesa.c provides the real implementation. */
EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                   const EGLint *attrib_list) {
  (void)dpy;
  (void)config;
  (void)attrib_list;
  g_error = EGL_BAD_SURFACE;
  return EGL_NO_SURFACE;
}

const uint32_t *eglB1nixSurfacePixels(EGLDisplay dpy, EGLSurface surface,
                                      EGLint *width, EGLint *height) {
  (void)dpy;
  struct b1egl_surface *s = (struct b1egl_surface *)surface;
  if (!s || !s->win)
    return NULL;
  if (width)
    *width = s->width;
  if (height)
    *height = s->height;
  return s->win->pixels;
}
