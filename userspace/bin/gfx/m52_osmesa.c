/* M52 end-to-end with real Mesa: OSMesa (off-screen Gallium softpipe, software
 * OpenGL) renders a 3D triangle straight into a b1gui wl_shm window and presents
 * it to displayd. This is the unmodified upstream Mesa OSMesa API on b1nix —
 * OSMesaCreateContext / OSMesaMakeCurrent driving the softpipe rasterizer. Every
 * marker is gated on the real rendered pixels. */
#include <GL/gl.h>
#include <GL/osmesa.h>
#include <b1nix/gui.h>
#include <stdint.h>
#include <stdlib.h>
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
    return fail("M52-GFX: fail mesa (connect)\n");
  if (b1gui_create_window(&win, 128, 96, "M52 Mesa") < 0 || !win.pixels)
    return fail("M52-GFX: fail mesa (window)\n");

  /* OSMESA_BGRA + GL_UNSIGNED_BYTE writes bytes B,G,R,A — i.e. ARGB32 in native
   * little-endian, exactly the wl_shm window format. */
  OSMesaContext ctx = OSMesaCreateContext(OSMESA_BGRA, NULL);
  if (!ctx)
    return fail("M52-GFX: fail mesa (context)\n");
  mark("M52-GFX: ok mesa-context\n");

  if (!OSMesaMakeCurrent(ctx, win.pixels, GL_UNSIGNED_BYTE, win.width,
                         win.height))
    return fail("M52-GFX: fail mesa (makecurrent)\n");
  /* Render top-down so row 0 is the top of the window (GL is bottom-up). */
  OSMesaPixelStore(OSMESA_Y_UP, 0);

  const char *ver = (const char *)glGetString(GL_VERSION);
  if (!ver)
    return fail("M52-GFX: fail mesa (version)\n");

  glViewport(0, 0, win.width, win.height);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
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
  glFinish(); /* softpipe renders into win.pixels */

  /* Inspect the rendered buffer: blue background + red triangle. ARGB32. */
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
    return fail("M52-GFX: fail mesa (no-clear)\n");
  if (red < 100)
    return fail("M52-GFX: fail mesa (no-triangle)\n");
  mark("M52-GFX: ok mesa-render\n");

  if (b1gui_present(&win, 0, 0, win.width, win.height) < 0)
    return fail("M52-GFX: fail mesa (present)\n");
  struct b1gui_event ev;
  for (int i = 0; i < 200; i++)
    if (b1gui_next_event(&win, &ev, 1000) == 1 && ev.type == B1GUI_EV_FRAME)
      break;
  mark("M52-GFX: ok mesa\n");

  OSMesaDestroyContext(ctx);
  b1gui_destroy(&win);
  return 0;
}
