#ifndef _EXECINFO_H
#define _EXECINFO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* <execinfo.h>: GNU backtrace API. b1nix has no userspace stack-unwinding
 * library, so these are honest no-ops: backtrace() captures 0 frames and the
 * symbol helpers return NULL / write nothing. Callers (swiftshader/crashpad
 * crash handlers) degrade to "no backtrace available". Added for the Chromium
 * port (M60-62). */
static inline int backtrace(void **buffer, int size) {
  (void)buffer; (void)size;
  return 0;
}
static inline char **backtrace_symbols(void *const *buffer, int size) {
  (void)buffer; (void)size;
  return (char **)0;
}
static inline void backtrace_symbols_fd(void *const *buffer, int size, int fd) {
  (void)buffer; (void)size; (void)fd;
}

#ifdef __cplusplus
}
#endif

#endif /* _EXECINFO_H */
