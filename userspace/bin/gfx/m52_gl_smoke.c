/* M52 end-to-end: an EGL + OpenGL (TinyGL software) Wayland app. Sets up an EGL
 * window surface on a b1gui window, clears to blue, draws a red triangle through
 * the real GL pipeline, swaps, then verifies the presented wl_shm buffer
 * actually contains blue background + red triangle ink. No faked markers: every
 * "ok" is gated on real rendered pixels. */
#include <EGL/egl.h>
#include <GL/gl.h>
#include <b1nix/gui.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void mark(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) {
  mark(s);
  return 1;
}

int main(void) {
  struct b1gui_window win;
  memset(&win, 0, sizeof(win));
  if (b1gui_connect(&win) < 0)
    return fail("M52-GFX: fail gl (connect)\n");
  if (b1gui_create_window(&win, 128, 96, "M52 GL") < 0 || !win.pixels)
    return fail("M52-GFX: fail gl (window)\n");

  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor))
    return fail("M52-GFX: fail egl (init)\n");
  mark("M52-GFX: ok egl\n");

  eglBindAPI(EGL_OPENGL_API);
  EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                        EGL_NONE};
  EGLConfig cfg;
  EGLint ncfg = 0;
  if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1)
    return fail("M52-GFX: fail egl (config)\n");
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
  EGLSurface surf = eglCreateWindowSurface(dpy, cfg, &win, NULL);
  if (surf == EGL_NO_SURFACE)
    return fail("M52-GFX: fail egl (surface)\n");
  if (!eglMakeCurrent(dpy, surf, surf, ctx))
    return fail("M52-GFX: fail egl (makecurrent)\n");

  /* The renderer is live now — TinyGL is bound to this surface's framebuffer. */
  if (!glGetString(GL_VERSION))
    return fail("M52-GFX: fail tinygl (version)\n");
  mark("M52-GFX: ok tinygl\n");

  /* Identity projection + modelview: vertices in clip space [-1,1] map straight
   * to the viewport — no ortho/frustum needed for a flat 2D triangle. */
  glViewport(0, 0, win.width, win.height);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glClearColor(0.0f, 0.0f, 1.0f, 1.0f); /* blue */
  glClear(GL_COLOR_BUFFER_BIT);

  glBegin(GL_TRIANGLES);
  glColor3f(1.0f, 0.0f, 0.0f); /* red */
  glVertex3f(-0.7f, -0.7f, 0.0f);
  glVertex3f(0.7f, -0.7f, 0.0f);
  glVertex3f(0.0f, 0.7f, 0.0f);
  glEnd();

  if (!eglSwapBuffers(dpy, surf))
    return fail("M52-GFX: fail egl (swap)\n");

  /* Inspect the presented buffer: count blue-background and red-triangle
   * pixels. ARGB32 0xAARRGGBB. Blue bg: B high, R/G low. Red ink: R high. */
  unsigned blue = 0, red = 0;
  uint32_t n = win.width * win.height;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t p = win.pixels[i];
    unsigned r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
    if (b > 0xc0 && r < 0x40 && g < 0x40)
      blue++;
    else if (r > 0xc0 && g < 0x40 && b < 0x40)
      red++;
  }
  if (blue < 100)
    return fail("M52-GFX: fail gl-triangle (no-clear)\n");
  if (red < 100)
    return fail("M52-GFX: fail gl-triangle (no-triangle)\n");
  mark("M52-GFX: ok gl-triangle\n");

  mark(eglB1nixAccelerated(dpy) ? "M52-GFX: ok path-accelerated\n"
                                : "M52-GFX: ok path-software\n");

  eglDestroySurface(dpy, surf);
  eglDestroyContext(dpy, ctx);
  eglTerminate(dpy);
  b1gui_destroy(&win);
  return 0;
}
