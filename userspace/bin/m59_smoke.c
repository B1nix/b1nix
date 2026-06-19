/* M59 end-to-end: a real EGL app over the b1nix OSMesa (Mesa softpipe) backend.
 * It runs the full standard EGL sequence entirely off-screen — no window, no
 * displayd — so it can live in the core smoke instance:
 *
 *   eglGetDisplay(EGL_DEFAULT_DISPLAY) -> eglInitialize -> eglChooseConfig
 *   -> eglCreateContext -> eglCreatePbufferSurface -> eglMakeCurrent
 *   -> GL clear-to-blue + draw a red triangle -> eglSwapBuffers
 *   -> read back the pbuffer color buffer and VERIFY the pixels.
 *
 * Every "ok" marker is gated on real rendered pixels (clear color present,
 * triangle ink present, center pixel is the triangle color). No faked markers. */
#include <EGL/egl.h>
#include <GL/gl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void mark(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) {
  mark(s);
  return 1;
}

#define W 64
#define H 64

int main(void) {
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor))
    return fail("M59-SMOKE: fail egl-init (initialize)\n");
  if (major < 1)
    return fail("M59-SMOKE: fail egl-init (version)\n");
  mark("M59-SMOKE: ok egl-init\n");

  if (!eglBindAPI(EGL_OPENGL_API))
    return fail("M59-SMOKE: fail egl-context (bindapi)\n");

  EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                        EGL_DEPTH_SIZE, 16, EGL_NONE};
  EGLConfig cfg;
  EGLint ncfg = 0;
  if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1)
    return fail("M59-SMOKE: fail egl-context (choose-config)\n");

  /* Verify the chosen config really advertises 8-bit RGBA. */
  EGLint rs = 0, as = 0;
  if (!eglGetConfigAttrib(dpy, cfg, EGL_RED_SIZE, &rs) ||
      !eglGetConfigAttrib(dpy, cfg, EGL_ALPHA_SIZE, &as) || rs < 8 || as < 8)
    return fail("M59-SMOKE: fail egl-context (config-attrib)\n");

  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
  if (ctx == EGL_NO_CONTEXT)
    return fail("M59-SMOKE: fail egl-context (create)\n");

  EGLint pb_attrs[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
  EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
  if (surf == EGL_NO_SURFACE)
    return fail("M59-SMOKE: fail egl-context (pbuffer)\n");

  if (!eglMakeCurrent(dpy, surf, surf, ctx))
    return fail("M59-SMOKE: fail egl-context (makecurrent)\n");

  /* GL is live now — softpipe is bound to the pbuffer color buffer. */
  if (!glGetString(GL_VERSION))
    return fail("M59-SMOKE: fail egl-context (gl-version)\n");
  mark("M59-SMOKE: ok egl-context\n");

  glViewport(0, 0, W, H);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glClearColor(0.0f, 0.0f, 1.0f, 1.0f); /* blue */
  glClear(GL_COLOR_BUFFER_BIT);

  glBegin(GL_TRIANGLES);
  glColor3f(1.0f, 0.0f, 0.0f); /* red */
  glVertex3f(-0.8f, -0.8f, 0.0f);
  glVertex3f(0.8f, -0.8f, 0.0f);
  glVertex3f(0.0f, 0.8f, 0.0f);
  glEnd();

  if (!eglSwapBuffers(dpy, surf))
    return fail("M59-SMOKE: fail egl-render (swap)\n");

  /* Read back the off-screen color buffer through the standard b1nix EGL
   * extension and verify the pixels. ARGB32 0xAARRGGBB. */
  EGLint rw = 0, rh = 0;
  const uint32_t *px = eglB1nixSurfacePixels(dpy, surf, &rw, &rh);
  if (!px || rw != W || rh != H)
    return fail("M59-SMOKE: fail egl-render (readback)\n");

  unsigned blue = 0, red = 0;
  uint32_t n = (uint32_t)W * (uint32_t)H;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t p = px[i];
    unsigned r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
    if (b > 0xc0 && r < 0x40 && g < 0x40)
      blue++;
    else if (r > 0xc0 && g < 0x40 && b < 0x40)
      red++;
  }
  if (blue < 100)
    return fail("M59-SMOKE: fail egl-render (no-clear)\n");
  if (red < 100)
    return fail("M59-SMOKE: fail egl-render (no-triangle)\n");

  /* Center pixel must be inside the triangle -> red. */
  uint32_t c = px[(H / 2) * W + (W / 2)];
  unsigned cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
  if (!(cr > 0xc0 && cg < 0x40 && cb < 0x40))
    return fail("M59-SMOKE: fail egl-render (center-not-triangle)\n");
  mark("M59-SMOKE: ok egl-render\n");

  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(dpy, surf);
  eglDestroyContext(dpy, ctx);
  eglTerminate(dpy);

  mark("M59-SMOKE: done\n");
  return 0;
}
