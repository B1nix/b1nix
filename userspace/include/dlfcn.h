#ifndef B1NIX_U_DLFCN_H
#define B1NIX_U_DLFCN_H

#include <stddef.h>   /* NULL */

/*
 * dlfcn.h — B1NIX userspace
 *
 * B1NIX supports static ELF binaries only.  Dynamic loading (dlopen/dlsym)
 * is NOT available at runtime.  These declarations are provided so that
 * third-party code that conditionally uses dlopen can compile cleanly.
 *
 * Runtime behaviour:
 *   dlopen(name, ...)  → always returns NULL (error stored in dlerror buffer)
 *   dlopen(NULL, ...)  → returns a non-NULL sentinel (RTLD_DEFAULT semantics)
 *   dlsym(h, sym)      → always returns NULL (error stored in dlerror buffer)
 *   dlclose(h)         → always returns -1  (error stored in dlerror buffer)
 *   dlerror()          → returns last error string and clears it (POSIX)
 */

/* Mode flags for dlopen() */
#define RTLD_LAZY       1   /* Relocations performed at first use   */
#define RTLD_NOW        2   /* All relocations at dlopen() time     */
#define RTLD_GLOBAL     4   /* Symbols available to later dlopen()  */
#define RTLD_LOCAL      0   /* Symbols not available (default)      */
#define RTLD_NOLOAD     8   /* Don't load, just check               */

/* Pseudo-handles for dlsym() */
#define RTLD_DEFAULT    ((void *)0)          /* Default symbol search */
#define RTLD_NEXT       ((void *)(unsigned long)(-1)) /* Next occurrence  */

void *dlopen(const char *filename, int flag);
char *dlerror(void);
void *dlsym(void *handle, const char *symbol);
int   dlclose(void *handle);

#endif /* B1NIX_U_DLFCN_H */
