/* M52 programmable shader pipeline with real Mesa: an OSMesa (off-screen Gallium
 * softpipe, software OpenGL) context drives a genuine GLSL shader program — a
 * vertex shader feeding a per-vertex colour varying into a fragment shader — and
 * rasterises a Gouraud-shaded triangle straight into a b1gui wl_shm window, then
 * presents it to displayd. This exercises the programmable GL 2.x pipeline
 * (glCreateShader/glCompileShader/glLinkProgram/glUseProgram, VBOs, generic
 * vertex attributes and varyings) — the surface beyond the fixed-function path
 * verified by m52_osmesa. Every marker is gated on the real rendered pixels:
 * the three triangle corners must come out red, green and blue, which can only
 * happen if the vertex attributes were interpolated through the varying and
 * emitted by the fragment shader. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/osmesa.h>
#include <GL/glext.h>
#include <b1nix/gui.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void mark(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) {
  mark(s);
  return 1;
}

/* GLSL 1.20 (desktop GL 2.1 / softpipe) — the GLSL ES 1.00 feature level:
 * generic attributes, a colour varying, a fragment-shader colour output. */
static const char *VERT_SRC =
    "#version 120\n"
    "attribute vec2 aPos;\n"
    "attribute vec3 aColor;\n"
    "varying vec3 vColor;\n"
    "void main() {\n"
    "  vColor = aColor;\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *FRAG_SRC =
    "#version 120\n"
    "varying vec3 vColor;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(vColor, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
  GLuint sh = glCreateShader(type);
  if (!sh)
    return 0;
  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

int main(void) {
  struct b1gui_window win;
  memset(&win, 0, sizeof(win));
  if (b1gui_connect(&win) < 0)
    return fail("M52-GFX: fail glsl (connect)\n");
  if (b1gui_create_window(&win, 128, 96, "M52 GLSL") < 0 || !win.pixels)
    return fail("M52-GFX: fail glsl (window)\n");

  /* OSMESA_BGRA + GL_UNSIGNED_BYTE writes B,G,R,A bytes = ARGB32 little-endian,
   * exactly the wl_shm window format. */
  OSMesaContext ctx = OSMesaCreateContext(OSMESA_BGRA, NULL);
  if (!ctx)
    return fail("M52-GFX: fail glsl (context)\n");
  if (!OSMesaMakeCurrent(ctx, win.pixels, GL_UNSIGNED_BYTE, win.width,
                         win.height))
    return fail("M52-GFX: fail glsl (makecurrent)\n");
  OSMesaPixelStore(OSMESA_Y_UP, 0); /* row 0 = top of window */

  /* The programmable pipeline requires GLSL support (GL >= 2.0). */
  if (!glGetString(GL_SHADING_LANGUAGE_VERSION))
    return fail("M52-GFX: fail glsl (no-glsl)\n");

  GLuint vs = compile(GL_VERTEX_SHADER, VERT_SRC);
  GLuint fs = compile(GL_FRAGMENT_SHADER, FRAG_SRC);
  if (!vs || !fs)
    return fail("M52-GFX: fail glsl (compile)\n");
  mark("M52-GFX: ok shader-compile\n");

  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  /* Bind attribute locations before linking so we know them deterministically. */
  glBindAttribLocation(prog, 0, "aPos");
  glBindAttribLocation(prog, 1, "aColor");
  glLinkProgram(prog);
  GLint linked = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &linked);
  if (!linked)
    return fail("M52-GFX: fail glsl (link)\n");
  mark("M52-GFX: ok shader-link\n");

  glUseProgram(prog);

  /* Interleaved [x, y, r, g, b] per vertex: a triangle whose three corners are
   * pure red, green and blue. The varying interpolates these across the face. */
  static const GLfloat verts[] = {
      0.0f,  0.8f,  1.0f, 0.0f, 0.0f, /* top    — red   */
      -0.8f, -0.8f, 0.0f, 1.0f, 0.0f, /* left   — green */
      0.8f,  -0.8f, 0.0f, 0.0f, 1.0f, /* right  — blue  */
  };
  GLuint vbo = 0;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                        (const void *)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                        (const void *)(2 * sizeof(GLfloat)));

  glViewport(0, 0, win.width, win.height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f); /* black background */
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glFinish(); /* softpipe runs the shaders into win.pixels */

  /* Inspect the rendered buffer (ARGB32). The three corners must come out as
   * dominant red, green and blue — only possible if attributes were passed
   * through the varying and written by the fragment shader. */
  unsigned red = 0, green = 0, blue = 0;
  uint32_t n = win.width * win.height;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t p = win.pixels[i];
    unsigned r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
    if (r > 0xb0 && g < 0x50 && b < 0x50)
      red++;
    else if (g > 0xb0 && r < 0x50 && b < 0x50)
      green++;
    else if (b > 0xb0 && r < 0x50 && g < 0x50)
      blue++;
  }
  if (red < 40 || green < 40 || blue < 40)
    return fail("M52-GFX: fail shader-render (corners)\n");
  mark("M52-GFX: ok shader-render\n");

  if (b1gui_present(&win, 0, 0, win.width, win.height) < 0)
    return fail("M52-GFX: fail glsl (present)\n");
  struct b1gui_event ev;
  for (int i = 0; i < 200; i++)
    if (b1gui_next_event(&win, &ev, 1000) == 1 && ev.type == B1GUI_EV_FRAME)
      break;
  mark("M52-GFX: ok glsl\n");

  glDeleteProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  OSMesaDestroyContext(ctx);
  b1gui_destroy(&win);
  return 0;
}
