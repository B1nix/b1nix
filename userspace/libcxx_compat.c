/* Shims and compatibility helpers for running LLVM libc++ against musl libc on b1nix. */
#include <stddef.h>

/* __cxa_thread_atexit_impl: Itanium C++ ABI helper for thread_local destructors.
 * Prebuilt libc++abi.so.1 expects this symbol to be provided by the C library.
 * Since musl does not provide it, we provide this stub. None of our current C++
 * smoke tests rely on thread_local destructors. */
int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dso_handle) {
  (void)func;
  (void)obj;
  (void)dso_handle;
  return 0;
}
