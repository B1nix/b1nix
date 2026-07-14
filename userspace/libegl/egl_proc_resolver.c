/*
 * egl_proc_resolver.c — eglGetProcAddress implementation for b1nix.
 *
 * Dawn (and other GL consumers) call eglGetProcAddress to resolve GL entry
 * points at runtime. On b1nix all GL functions are statically linked from
 * Mesa's libglapi_static.a, so we resolve via dlsym(RTLD_DEFAULT, name).
 *
 * This file is compiled into libEGL.so alongside b1egl_mesa.c.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>

typedef void (*EGLProc)(void);

EGLProc eglGetProcAddress(const char *name) {
    if (!name) return NULL;
    /* dlsym on RTLD_DEFAULT finds any symbol loaded in the process —
     * Mesa's gl* functions from libglapi_static.a are all there. */
    return (EGLProc)dlsym(RTLD_DEFAULT, name);
}
