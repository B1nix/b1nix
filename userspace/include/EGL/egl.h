/* Minimal EGL 1.4 surface for b1nix, backed by TinyGL software rendering into a
 * b1gui (Wayland) window. Only the entry points a windowed GL app actually
 * uses are declared; this is a real implementation in b1egl.c, not a stub. */
#ifndef B1NIX_EGL_H
#define B1NIX_EGL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType; /* b1nix: a struct b1gui_window * */
typedef unsigned int EGLBoolean;
typedef int EGLint;
typedef unsigned int EGLenum;

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)

#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_SURFACE 0x300D

#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_OPENGL_ES_BIT 0x0001
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054

#define EGL_OPENGL_API 0x30A2
#define EGL_OPENGL_ES_API 0x30A0

/* EGL 1.4/1.5 additions used by the OSMesa-backed (off-screen pbuffer) path
 * implemented in b1egl_mesa.c. Window-only TinyGL (b1egl.c) ignores these. */
#define EGL_BAD_CONFIG 0x3005
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_PARAMETER 0x300C
#define EGL_PBUFFER_BIT 0x0001
#define EGL_CONFIG_ID 0x3028
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_BUFFER_SIZE 0x3020
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SAMPLES 0x3031
#define EGL_CONFORMANT 0x3042
#define EGL_CLIENT_APIS 0x308D
#define EGL_EXTENSIONS 0x3055
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_LARGEST_PBUFFER 0x3058

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id);
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean eglTerminate(EGLDisplay dpy);
const char *eglQueryString(EGLDisplay dpy, EGLint name);
EGLBoolean eglBindAPI(EGLenum api);
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size,
                           EGLint *num_config);
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute,
                              EGLint *value);
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context, const EGLint *attrib_list);
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win,
                                  const EGLint *attrib_list);
EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                           EGLint *value);
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                          EGLContext ctx);
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLint eglGetError(void);

/* Off-screen rendering target. Backed by an OSMesa color buffer (b1egl_mesa.c);
 * a no-op stub in the TinyGL window-only build (b1egl.c). */
EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                   const EGLint *attrib_list);

/* b1nix extension: which path eglSwapBuffers used last (1 = GPU/VirGL,
 * 0 = software). Lets a smoke test verify acceleration kicked in. */
int eglB1nixAccelerated(EGLDisplay dpy);

/* b1nix extension: return a pointer to the surface's CPU-visible color buffer
 * (ARGB32, 0xAARRGGBB, row-major top-down). NULL if unsupported. Lets an
 * off-screen smoke read back rendered pixels without a window/displayd. */
const uint32_t *eglB1nixSurfacePixels(EGLDisplay dpy, EGLSurface surface,
                                      EGLint *width, EGLint *height);

#ifdef __cplusplus
}
#endif
#endif
