/*
 * Does the GPU draw, or does the driver merely start?
 *
 * Everything the i915 work had shown until now stopped one step short of an
 * answer: Mesa's iris loads, EGL initialises, wlroots prints "Mesa Intel(R) UHD
 * Graphics 630" — and none of that touches the render engine. A driver that
 * enumerates the device, builds a context and never successfully submits a
 * batch prints exactly the same lines. "Initialised" and "rendered" are two
 * different claims, and only pixels settle the second one.
 *
 * So this asks for pixels, off-screen, with no compositor and no display:
 *
 *   1. an EGL display on the DRM device itself (EGL_PLATFORM_DEVICE_EXT), so
 *      the run cannot silently fall through to a software rasteriser on the
 *      default display — and GL_RENDERER is checked afterwards anyway, because
 *      llvmpipe would answer every call here perfectly.
 *   2. a 64x64 renderbuffer, cleared to a colour chosen so that no channel
 *      equals another: a readback of the wrong format, a swizzle or a stride
 *      mistake all survive a clear to grey.
 *   3. a triangle covering the lower-left half, through a compiled shader.
 *      The clear alone could in principle be served without the 3D pipeline;
 *      a rasterised triangle needs the shader compiler (iris's own NIR
 *      backend), a batch, and the GPU to execute it.
 *   4. glReadPixels, and two pixels checked by hand — one inside the triangle,
 *      one outside it. Both wrong is a dead pipeline; the triangle's pixel
 *      alone wrong is a rasteriser or shader fault; the clear's alone wrong is
 *      a clear that never landed. They fail apart, so they are reported apart.
 *
 * Reached through dlopen, exactly as bin/gfx/font_probe.c is and for the same
 * reason: this then runs against the very shared objects the compositor loads,
 * needs no GL headers at build time, and cannot accidentally be answered by a
 * different copy of Mesa linked in here.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* EGL and GL enumerants, spelled out rather than included. The values are ABI
 * — they are what the shared object was compiled against — so a header would
 * add a build dependency without adding a guarantee. */
#define EGL_NONE 0x3038
#define EGL_TRUE 1
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_EXTENSIONS 0x3055
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_RGBA8 0x8058
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004
#define GL_FALSE 0

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLDeviceEXT;

typedef void *(*fn_getprocaddr)(const char *);
typedef unsigned (*fn_eglInitialize)(EGLDisplay, int *, int *);
typedef unsigned (*fn_eglBindAPI)(unsigned);
typedef const char *(*fn_eglQueryString)(EGLDisplay, int);
typedef EGLContext (*fn_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                          const int *);
typedef unsigned (*fn_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
                                      EGLContext);
typedef unsigned (*fn_eglChooseConfig)(EGLDisplay, const int *, EGLConfig *,
                                       int, int *);
typedef unsigned (*fn_eglGetError)(void);
typedef EGLDisplay (*fn_eglGetDisplay)(void *);
typedef EGLDisplay (*fn_eglGetPlatformDisplayEXT)(unsigned, void *,
                                                  const int *);
typedef unsigned (*fn_eglQueryDevicesEXT)(int, EGLDeviceEXT *, int *);
typedef const char *(*fn_eglQueryDeviceStringEXT)(EGLDeviceEXT, int);

typedef void (*fn_v_iu)(int, unsigned *);
typedef void (*fn_v_uu)(unsigned, unsigned);
typedef void (*fn_glRenderbufferStorage)(unsigned, unsigned, int, int);
typedef void (*fn_glFramebufferRenderbuffer)(unsigned, unsigned, unsigned,
                                             unsigned);
typedef unsigned (*fn_glCheckFramebufferStatus)(unsigned);
typedef void (*fn_glViewport)(int, int, int, int);
typedef void (*fn_glClearColor)(float, float, float, float);
typedef void (*fn_glClear)(unsigned);
typedef void (*fn_glFinish)(void);
typedef void (*fn_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
typedef const unsigned char *(*fn_glGetString)(unsigned);
typedef unsigned (*fn_glGetError)(void);
typedef unsigned (*fn_glCreateShader)(unsigned);
typedef void (*fn_glShaderSource)(unsigned, int, const char *const *,
                                  const int *);
typedef void (*fn_glCompileShader)(unsigned);
typedef void (*fn_glGetShaderiv)(unsigned, unsigned, int *);
typedef void (*fn_glGetShaderInfoLog)(unsigned, int, int *, char *);
typedef unsigned (*fn_glCreateProgram)(void);
typedef void (*fn_glAttachShader)(unsigned, unsigned);
typedef void (*fn_glBindAttribLocation)(unsigned, unsigned, const char *);
typedef void (*fn_glLinkProgram)(unsigned);
typedef void (*fn_glGetProgramiv)(unsigned, unsigned, int *);
typedef void (*fn_glGetProgramInfoLog)(unsigned, int, int *, char *);
typedef void (*fn_glUseProgram)(unsigned);
typedef void (*fn_glVertexAttribPointer)(unsigned, int, unsigned, unsigned, int,
                                         const void *);
typedef void (*fn_glEnableVertexAttribArray)(unsigned);
typedef void (*fn_glDrawArrays)(unsigned, int, int);

#define NEED(handle, type, name)                                              \
  type name = (type)dlsym(handle, #name);                                     \
  if (!name) {                                                                \
    printf("GL-PROBE: fail dlsym %s\n", #name);                               \
    return 1;                                                                 \
  }

/* A channel matches if it is within two of what was asked for. The fragment
 * shader's output is mediump, and 0.5 through a mediump float and back out as
 * eight bits is allowed to land on 127 or 128 — a mismatch of one is the
 * hardware being within spec, not a wrong pixel. */
static int near(unsigned char got, int want) {
  int d = (int)got - want;
  return d <= 2 && d >= -2;
}

static int check_pixel(const char *what, const unsigned char *p, int r, int g,
                       int b) {
  int ok = near(p[0], r) && near(p[1], g) && near(p[2], b);

  printf("GL-PROBE: %s pixel rgba=%u,%u,%u,%u want=%d,%d,%d -> %s\n", what,
         (unsigned)p[0], (unsigned)p[1], (unsigned)p[2], (unsigned)p[3], r, g, b,
         ok ? "ok" : "WRONG");
  return ok;
}

int main(int argc, char **argv) {
  /* Which node to render on. Named rather than discovered, because "EGL picked
   * a device" and "EGL picked THE device" are different results and only the
   * second one says anything about the passed-through GPU. */
  const char *want_node = argc > 1 ? argv[1] : NULL;
  void *egl = dlopen("libEGL.so.1", RTLD_NOW);
  void *gles = dlopen("libGLESv2.so.2", RTLD_NOW);

  if (!egl) {
    printf("GL-PROBE: fail dlopen libEGL: %s\n", dlerror());
    return 1;
  }
  if (!gles) {
    printf("GL-PROBE: fail dlopen libGLESv2: %s\n", dlerror());
    return 1;
  }

  NEED(egl, fn_getprocaddr, eglGetProcAddress)
  NEED(egl, fn_eglInitialize, eglInitialize)
  NEED(egl, fn_eglBindAPI, eglBindAPI)
  NEED(egl, fn_eglQueryString, eglQueryString)
  NEED(egl, fn_eglCreateContext, eglCreateContext)
  NEED(egl, fn_eglMakeCurrent, eglMakeCurrent)
  NEED(egl, fn_eglChooseConfig, eglChooseConfig)
  NEED(egl, fn_eglGetError, eglGetError)
  NEED(egl, fn_eglGetDisplay, eglGetDisplay)

  /* The device enumeration is an extension, so it comes through
   * eglGetProcAddress and not through dlsym: an EGL built without it exports
   * no such symbol, and the fallback below is the honest answer. */
  fn_eglGetPlatformDisplayEXT eglGetPlatformDisplayEXT =
      (fn_eglGetPlatformDisplayEXT)eglGetProcAddress("eglGetPlatformDisplayEXT");
  fn_eglQueryDevicesEXT eglQueryDevicesEXT =
      (fn_eglQueryDevicesEXT)eglGetProcAddress("eglQueryDevicesEXT");
  fn_eglQueryDeviceStringEXT eglQueryDeviceStringEXT =
      (fn_eglQueryDeviceStringEXT)eglGetProcAddress("eglQueryDeviceStringEXT");

  EGLDisplay dpy = NULL;

  if (eglGetPlatformDisplayEXT && eglQueryDevicesEXT &&
      eglQueryDeviceStringEXT) {
    EGLDeviceEXT devs[16];
    int n = 0;

    if (eglQueryDevicesEXT(16, devs, &n) == EGL_TRUE && n > 0) {
      int i;

      for (i = 0; i < n; i++) {
        const char *card = eglQueryDeviceStringEXT(devs[i],
                                                   EGL_DRM_DEVICE_FILE_EXT);
        const char *rnode =
            eglQueryDeviceStringEXT(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT);

        printf("GL-PROBE: device %d card=%s render=%s\n", i,
               card ? card : "(none)", rnode ? rnode : "(none)");
        if (dpy)
          continue;
        if (want_node) {
          if ((card && !strcmp(card, want_node)) ||
              (rnode && !strcmp(rnode, want_node)))
            dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devs[i],
                                           NULL);
        } else if (card || rnode) {
          /* Any DRM device, but a DRM device: the software device EGL also
           * enumerates carries neither string, and picking it would answer a
           * question nobody asked. */
          dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devs[i],
                                         NULL);
        }
      }
    } else {
      printf("GL-PROBE: eglQueryDevicesEXT found none (err 0x%x)\n",
             eglGetError());
    }
  } else {
    printf("GL-PROBE: no EGL_EXT_device_query in this libEGL\n");
  }

  if (!dpy) {
    printf("GL-PROBE: no DRM EGL device%s; falling back to the default display "
           "(this is very likely software)\n",
           want_node ? " matching the request" : "");
    dpy = eglGetDisplay(NULL);
  }
  if (!dpy) {
    printf("GL-PROBE: fail no-display\n");
    return 1;
  }

  int major = 0, minor = 0;

  if (eglInitialize(dpy, &major, &minor) != EGL_TRUE) {
    printf("GL-PROBE: fail eglInitialize err=0x%x\n", eglGetError());
    return 1;
  }
  printf("GL-PROBE: ok egl %d.%d\n", major, minor);

  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    printf("GL-PROBE: fail eglBindAPI err=0x%x\n", eglGetError());
    return 1;
  }

  /* A context with no config and no surface. Both are extensions, both are
   * advertised by this Mesa, and together they are what makes this probe
   * independent of any display: no pbuffer, no window, no compositor. If a
   * future Mesa drops either, the config path below is the fallback. */
  const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
  int no_config = exts && strstr(exts, "EGL_KHR_no_config_context") != NULL;
  int surfaceless = exts && strstr(exts, "EGL_KHR_surfaceless_context") != NULL;

  printf("GL-PROBE: no_config_context=%d surfaceless_context=%d\n", no_config,
         surfaceless);
  if (!surfaceless) {
    printf("GL-PROBE: fail no-surfaceless-context\n");
    return 1;
  }

  EGLConfig cfg = NULL;

  if (!no_config) {
    const int cfg_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
                             EGL_NONE};
    int ncfg = 0;

    if (eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) != EGL_TRUE ||
        ncfg < 1) {
      printf("GL-PROBE: fail eglChooseConfig err=0x%x\n", eglGetError());
      return 1;
    }
  }

  const int ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, NULL, ctx_attrs);

  if (!ctx) {
    printf("GL-PROBE: fail eglCreateContext err=0x%x\n", eglGetError());
    return 1;
  }
  if (eglMakeCurrent(dpy, NULL, NULL, ctx) != EGL_TRUE) {
    printf("GL-PROBE: fail eglMakeCurrent err=0x%x\n", eglGetError());
    return 1;
  }
  printf("GL-PROBE: ok context\n");

  NEED(gles, fn_glGetString, glGetString)
  NEED(gles, fn_glGetError, glGetError)
  NEED(gles, fn_v_iu, glGenFramebuffers)
  NEED(gles, fn_v_uu, glBindFramebuffer)
  NEED(gles, fn_glRenderbufferStorage, glRenderbufferStorage)
  NEED(gles, fn_glFramebufferRenderbuffer, glFramebufferRenderbuffer)
  NEED(gles, fn_glCheckFramebufferStatus, glCheckFramebufferStatus)
  NEED(gles, fn_glViewport, glViewport)
  NEED(gles, fn_glClearColor, glClearColor)
  NEED(gles, fn_glClear, glClear)
  NEED(gles, fn_glFinish, glFinish)
  NEED(gles, fn_glReadPixels, glReadPixels)
  NEED(gles, fn_glCreateShader, glCreateShader)
  NEED(gles, fn_glShaderSource, glShaderSource)
  NEED(gles, fn_glCompileShader, glCompileShader)
  NEED(gles, fn_glGetShaderiv, glGetShaderiv)
  NEED(gles, fn_glGetShaderInfoLog, glGetShaderInfoLog)
  NEED(gles, fn_glCreateProgram, glCreateProgram)
  NEED(gles, fn_glAttachShader, glAttachShader)
  NEED(gles, fn_glBindAttribLocation, glBindAttribLocation)
  NEED(gles, fn_glLinkProgram, glLinkProgram)
  NEED(gles, fn_glGetProgramiv, glGetProgramiv)
  NEED(gles, fn_glGetProgramInfoLog, glGetProgramInfoLog)
  NEED(gles, fn_glUseProgram, glUseProgram)
  NEED(gles, fn_glVertexAttribPointer, glVertexAttribPointer)
  NEED(gles, fn_glEnableVertexAttribArray, glEnableVertexAttribArray)
  NEED(gles, fn_glDrawArrays, glDrawArrays)
  /* Renderbuffer objects share glGenFramebuffers' signature. */
  fn_v_iu glGenRenderbuffers = (fn_v_iu)dlsym(gles, "glGenRenderbuffers");
  fn_v_uu glBindRenderbuffer = (fn_v_uu)dlsym(gles, "glBindRenderbuffer");

  if (!glGenRenderbuffers || !glBindRenderbuffer) {
    printf("GL-PROBE: fail dlsym renderbuffer entry points\n");
    return 1;
  }

  const unsigned char *vendor = glGetString(GL_VENDOR);
  const unsigned char *renderer = glGetString(GL_RENDERER);
  const unsigned char *version = glGetString(GL_VERSION);

  printf("GL-PROBE: vendor   %s\n", vendor ? (const char *)vendor : "(null)");
  printf("GL-PROBE: renderer %s\n",
         renderer ? (const char *)renderer : "(null)");
  printf("GL-PROBE: version  %s\n",
         version ? (const char *)version : "(null)");

  /* Which driver answered. llvmpipe and softpipe pass every check below
   * perfectly and say nothing whatever about the GPU, so the renderer string
   * is part of the result and not a decoration.
   *
   * Named by what is NOT a GPU rather than by which GPU is expected: an
   * allow-list of one vendor ("Intel") called virgl on a virtio-gpu, and
   * radeonsi, and everything else, software — and this probe is what the
   * compositor's renderer selection asks before it chooses the accelerated
   * path, so a wrong "no" there costs the hardware path on every machine that
   * is not the one machine the string was written for. */
  static const char *const software_renderers[] = {
      "llvmpipe", "softpipe", "swrast", "SwiftShader", "Software Rasterizer",
  };
  int hardware = renderer != NULL;
  for (size_t i = 0;
       hardware && i < sizeof(software_renderers) / sizeof(software_renderers[0]);
       i++)
    if (strstr((const char *)renderer, software_renderers[i]) != NULL)
      hardware = 0;

  printf(hardware ? "GL-PROBE: ok hardware-renderer\n"
                  : "GL-PROBE: SOFTWARE renderer — the rest proves Mesa, not "
                    "the GPU\n");

  /* Two sizes, and the second one is the point.
   *
   * A compositor's first submissions succeed here and its third fails with
   * ENOSPC after seven seconds, so the question is what separates them: how
   * much the batch asks the driver to bind, or something only a compositor
   * does. A 64x64 target and a 1920x1080 one run the *same* code through the
   * same context; if only the large one fails, the answer is size, and the
   * compositor is not special. They are reported separately for that reason.
   */
  static const int sizes[][2] = {{64, 64}, {1920, 1080}};
  int passes_ok = 0;
  unsigned pass;

  /* Compiled once and reused across sizes: a shader that compiles at one
   * resolution and not another would be a strange fault, and re-linking per
   * pass would make the two runs differ in more than the thing under test. */
  static const char *vs_src =
      "attribute vec2 pos;\n"
      "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
  static const char *fs_src =
      "precision mediump float;\n"
      "void main() { gl_FragColor = vec4(1.0, 0.0, 0.5, 1.0); }\n";
  unsigned vs = glCreateShader(GL_VERTEX_SHADER);
  unsigned fs = glCreateShader(GL_FRAGMENT_SHADER);
  int ok = 0;
  char log[512];

  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);
  glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glGetShaderInfoLog(vs, (int)sizeof(log), NULL, log);
    printf("GL-PROBE: fail vertex shader: %s\n", log);
    return 1;
  }
  glShaderSource(fs, 1, &fs_src, NULL);
  glCompileShader(fs);
  glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glGetShaderInfoLog(fs, (int)sizeof(log), NULL, log);
    printf("GL-PROBE: fail fragment shader: %s\n", log);
    return 1;
  }

  unsigned prog = glCreateProgram();

  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glBindAttribLocation(prog, 0, "pos");
  glLinkProgram(prog);
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    glGetProgramInfoLog(prog, (int)sizeof(log), NULL, log);
    printf("GL-PROBE: fail link: %s\n", log);
    return 1;
  }
  printf("GL-PROBE: ok shader-compile-and-link\n");

  for (pass = 0; pass < sizeof(sizes) / sizeof(sizes[0]); pass++) {
    const int W = sizes[pass][0];
    const int H = sizes[pass][1];
    unsigned fbo = 0, rbo = 0;
    unsigned char *px = malloc((size_t)W * (size_t)H * 4);

    if (!px) {
      printf("GL-PROBE: fail %dx%d out of guest memory for the readback\n", W,
             H);
      return 1;
    }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rbo);

    unsigned status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
      printf("GL-PROBE: fail %dx%d framebuffer status=0x%x\n", W, H, status);
      free(px);
      continue;
    }
    printf("GL-PROBE: ok framebuffer %dx%d\n", W, H);

    glViewport(0, 0, W, H);
    /* Three different channels: a readback that swaps two of them, or reports
     * a different format than it produced, cannot pass by accident. */
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

    unsigned err = glGetError();

    if (err) {
      printf("GL-PROBE: fail %dx%d gl error 0x%x after clear+readback\n", W, H,
             err);
      free(px);
      continue;
    }
    /* 0.25/0.5/0.75 in eight bits. */
    int clear_ok =
        check_pixel("clear", &px[(H / 2) * W * 4 + (W / 2) * 4], 64, 128, 191);

    /* The lower-left half of the target, so one corner is covered and the
     * opposite corner keeps the clear colour. A full-screen triangle would
     * make a shader that emits the clear colour look like success. */
    static const float verts[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f};

    glUseProgram(prog);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    err = glGetError();
    if (err) {
      printf("GL-PROBE: fail %dx%d gl error 0x%x after draw\n", W, H, err);
      free(px);
      continue;
    }

    int inside = check_pixel("triangle", &px[2 * W * 4 + 2 * 4], 255, 0, 128);
    int outside = check_pixel("background",
                              &px[(H - 3) * W * 4 + (W - 3) * 4], 64, 128, 191);

    printf("GL-PROBE: %s %dx%d clear=%d triangle=%d\n",
           (clear_ok && inside && outside) ? "ok" : "fail", W, H, clear_ok,
           inside && outside);
    if (clear_ok && inside && outside)
      passes_ok++;
    free(px);
  }

  printf("GL-PROBE: done passes=%d/%u hardware=%d\n", passes_ok,
         (unsigned)(sizeof(sizes) / sizeof(sizes[0])), hardware);
  return (passes_ok == (int)(sizeof(sizes) / sizeof(sizes[0])) && hardware) ? 0
                                                                            : 1;
}
