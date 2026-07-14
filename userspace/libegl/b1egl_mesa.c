/* b1nix EGL 1.4/1.5 implementation backed by real Mesa OSMesa (off-screen
 * Gallium softpipe, software OpenGL). This is the standard egl* entry layer the
 * M59 milestone asks for: an app does the normal eglInitialize / eglChooseConfig
 * / eglCreateContext / eglCreate{Window,Pbuffer}Surface / eglMakeCurrent / GL
 * draws / eglSwapBuffers sequence, and the GL underneath is the unmodified Mesa
 * softpipe rasterizer (the same one M52 m52_osmesa drives directly).
 *
 * Two surface kinds:
 *   - Pbuffer  : off-screen. We allocate the color buffer ourselves (the small
 *                "GBM"-shaped part — just a CPU-visible ARGB32 buffer) and
 *                OSMesaMakeCurrent renders straight into it. eglSwapBuffers is a
 *                glFinish() + flush; the buffer is read back with
 *                eglB1nixSurfacePixels(). No displayd, no window needed.
 *   - Window   : on-screen. The color buffer is the live b1gui (Wayland) window
 *                wl_shm buffer; eglSwapBuffers glFinish()es then presents it to
 *                displayd, exactly like the M52 OSMesa demo.
 *
 * The color format is OSMESA_BGRA + GL_UNSIGNED_BYTE, which in little-endian
 * writes bytes B,G,R,A == ARGB32 0xAARRGGBB, matching the wl_shm window format.
 *
 * One Mesa context per EGLContext; OSMesa keeps the bind in the context itself,
 * so eglMakeCurrent simply re-binds the current surface's buffer. */
#include <EGL/egl.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/osmesa.h>

#include <b1nix/gui.h>

enum b1egl_kind { B1EGL_PBUFFER, B1EGL_WINDOW };

struct b1egl_surface {
  enum b1egl_kind kind;
  int width, height;
  uint32_t *pixels;          /* ARGB32 color buffer (OSMesa render target) */
  int owns_pixels;           /* pbuffer: we malloc'd it; window: borrowed */
  struct b1gui_window *win;  /* window surface only */
};

struct b1egl_context {
  OSMesaContext osmesa;
};

static int g_inited;
static EGLint g_error = EGL_SUCCESS;
static struct b1egl_context *g_current_ctx;
static struct b1egl_surface *g_current_surf;

/* Opaque non-NULL handles. The display and config are singletons. */
static EGLDisplay kDisplay = (EGLDisplay)1;
static EGLConfig kConfig = (EGLConfig)1;

static void set_error(EGLint e) { g_error = e; }

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id; /* EGL_DEFAULT_DISPLAY -> the one b1nix display */
  return kDisplay;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  if (dpy != kDisplay) {
    set_error(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  g_inited = 1;
  if (major)
    *major = 1;
  if (minor)
    *minor = 5;
  set_error(EGL_SUCCESS);
  return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
  if (dpy != kDisplay) {
    set_error(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  g_inited = 0;
  return EGL_TRUE;
}

const char *eglQueryString(EGLDisplay dpy, EGLint name) {
  (void)dpy;
  switch (name) {
  case EGL_VENDOR:
    return "b1nix";
  case EGL_VERSION:
    return "1.5 (Mesa OSMesa softpipe)";
  case EGL_CLIENT_APIS:
    return "OpenGL";
  case EGL_EXTENSIONS:
    return "";
  default:
    return "";
  }
}

EGLBoolean eglBindAPI(EGLenum api) {
  /* OSMesa is desktop OpenGL only. */
  if (api == EGL_OPENGL_API)
    return EGL_TRUE;
  set_error(EGL_BAD_PARAMETER);
  return EGL_FALSE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size,
                           EGLint *num_config) {
  if (dpy != kDisplay) {
    set_error(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  /* Single 8/8/8/8 + 16-bit depth softpipe config; it satisfies any sane
   * RGBA/depth request, so we accept the attrib list rather than filter it. */
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
  if (!value) {
    set_error(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  switch (attribute) {
  case EGL_RED_SIZE:
  case EGL_GREEN_SIZE:
  case EGL_BLUE_SIZE:
  case EGL_ALPHA_SIZE:
    *value = 8;
    break;
  case EGL_BUFFER_SIZE:
    *value = 32;
    break;
  case EGL_DEPTH_SIZE:
    *value = 16;
    break;
  case EGL_STENCIL_SIZE:
    *value = 8;
    break;
  case EGL_SAMPLES:
    *value = 0;
    break;
  case EGL_SURFACE_TYPE:
    *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
    break;
  case EGL_RENDERABLE_TYPE:
  case EGL_CONFORMANT:
    *value = EGL_OPENGL_BIT;
    break;
  case EGL_CONFIG_ID:
    *value = 1;
    break;
  case EGL_NATIVE_VISUAL_ID:
    *value = 0;
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
  (void)config;
  (void)attrib_list;
  if (dpy != kDisplay) {
    set_error(EGL_BAD_DISPLAY);
    return EGL_NO_CONTEXT;
  }
  struct b1egl_context *c = calloc(1, sizeof(*c));
  if (!c) {
    set_error(EGL_BAD_PARAMETER);
    return EGL_NO_CONTEXT;
  }
  OSMesaContext share =
      share_context != EGL_NO_CONTEXT
          ? ((struct b1egl_context *)share_context)->osmesa
          : NULL;
  /* OSMESA_BGRA writes ARGB32 in little-endian, matching the wl_shm window. */
  c->osmesa = OSMesaCreateContext(OSMESA_BGRA, share);
  if (!c->osmesa) {
    free(c);
    set_error(EGL_BAD_CONFIG);
    return EGL_NO_CONTEXT;
  }
  return (EGLContext)c;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  struct b1egl_context *c = (struct b1egl_context *)ctx;
  if (!c)
    return EGL_FALSE;
  if (g_current_ctx == c)
    g_current_ctx = NULL;
  if (c->osmesa)
    OSMesaDestroyContext(c->osmesa);
  free(c);
  return EGL_TRUE;
}

static EGLint attr_get(const EGLint *list, EGLint key, EGLint dflt) {
  if (!list)
    return dflt;
  for (const EGLint *p = list; p[0] != EGL_NONE; p += 2)
    if (p[0] == key)
      return p[1];
  return dflt;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                   const EGLint *attrib_list) {
  (void)config;
  if (dpy != kDisplay) {
    set_error(EGL_BAD_DISPLAY);
    return EGL_NO_SURFACE;
  }
  int w = (int)attr_get(attrib_list, EGL_WIDTH, 64);
  int h = (int)attr_get(attrib_list, EGL_HEIGHT, 64);
  if (w <= 0 || h <= 0) {
    set_error(EGL_BAD_PARAMETER);
    return EGL_NO_SURFACE;
  }
  struct b1egl_surface *s = calloc(1, sizeof(*s));
  if (!s) {
    set_error(EGL_BAD_SURFACE);
    return EGL_NO_SURFACE;
  }
  /* Allocate the off-screen color buffer ourselves — the minimal GBM-shaped
   * step: a CPU-visible ARGB32 image the softpipe renders into. */
  s->pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
  if (!s->pixels) {
    free(s);
    set_error(EGL_BAD_SURFACE);
    return EGL_NO_SURFACE;
  }
  s->kind = B1EGL_PBUFFER;
  s->width = w;
  s->height = h;
  s->owns_pixels = 1;
  return (EGLSurface)s;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win,
                                  const EGLint *attrib_list) {
  (void)config;
  (void)attrib_list;
  if (dpy != kDisplay) {
    set_error(EGL_BAD_DISPLAY);
    return EGL_NO_SURFACE;
  }
  struct b1gui_window *w = (struct b1gui_window *)win;
  if (!w || !w->pixels) {
    set_error(EGL_BAD_SURFACE);
    return EGL_NO_SURFACE;
  }
  struct b1egl_surface *s = calloc(1, sizeof(*s));
  if (!s) {
    set_error(EGL_BAD_SURFACE);
    return EGL_NO_SURFACE;
  }
  s->kind = B1EGL_WINDOW;
  s->win = w;
  s->width = (int)w->width;
  s->height = (int)w->height;
  s->pixels = w->pixels; /* render straight into the live wl_shm buffer */
  s->owns_pixels = 0;
  return (EGLSurface)s;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  struct b1egl_surface *s = (struct b1egl_surface *)surface;
  if (!s)
    return EGL_FALSE;
  if (g_current_surf == s)
    g_current_surf = NULL;
  if (s->owns_pixels)
    free(s->pixels);
  free(s);
  return EGL_TRUE;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                           EGLint *value) {
  (void)dpy;
  struct b1egl_surface *s = (struct b1egl_surface *)surface;
  if (!s || !value) {
    set_error(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
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
  struct b1egl_context *c = (struct b1egl_context *)ctx;
  struct b1egl_surface *s = (struct b1egl_surface *)draw;
  /* Release the binding. */
  if (c == EGL_NO_CONTEXT || s == EGL_NO_SURFACE) {
    g_current_ctx = NULL;
    g_current_surf = NULL;
    return EGL_TRUE;
  }
  if (!OSMesaMakeCurrent(c->osmesa, s->pixels, GL_UNSIGNED_BYTE, s->width,
                         s->height)) {
    set_error(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
  /* Row 0 is the top of the image (GL is bottom-up by default). */
  OSMesaPixelStore(OSMESA_Y_UP, 0);
  g_current_ctx = c;
  g_current_surf = s;
  return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  struct b1egl_surface *s = (struct b1egl_surface *)surface;
  if (!s) {
    set_error(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
  /* Flush the softpipe so all draws have landed in the color buffer. */
  glFinish();

  if (s->kind == B1EGL_PBUFFER)
    return EGL_TRUE; /* off-screen: nothing to present; read back the buffer */

  /* Window: the buffer is already the live wl_shm window; ensure opaque alpha
   * and present to displayd, then wait for the composited frame. */
  if (!s->win || !s->win->pixels) {
    set_error(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
  uint32_t *dst = s->win->pixels;
  uint32_t n = (uint32_t)s->width * (uint32_t)s->height;
  for (uint32_t i = 0; i < n; i++)
    dst[i] |= 0xff000000u;
  if (b1gui_present(s->win, 0, 0, (uint32_t)s->width, (uint32_t)s->height) < 0) {
    set_error(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
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

EGLDisplay eglGetCurrentDisplay(void) {
  if (!g_current_ctx) return EGL_NO_DISPLAY;
  return kDisplay;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw) {
  (void)readdraw;
  return (EGLSurface)g_current_surf;
}

EGLContext eglGetCurrentContext(void) {
  return (EGLContext)g_current_ctx;
}

int eglB1nixAccelerated(EGLDisplay dpy) {
  (void)dpy;
  return 0; /* OSMesa softpipe is software; virgl backend would override. */
}

const uint32_t *eglB1nixSurfacePixels(EGLDisplay dpy, EGLSurface surface,
                                      EGLint *width, EGLint *height) {
  (void)dpy;
  struct b1egl_surface *s = (struct b1egl_surface *)surface;
  if (!s || !s->pixels)
    return NULL;
  if (width)
    *width = s->width;
  if (height)
    *height = s->height;
  return s->pixels;
}
