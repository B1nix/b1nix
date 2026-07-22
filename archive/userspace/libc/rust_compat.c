/* rust_compat.c — libc symbols the Rust standard library expects.
 *
 * Rust std targets b1nix via a custom target spec with os=linux,env=musl
 * (it reuses std::sys::pal::unix unchanged and links this libc). A handful
 * of symbols std references are normally provided by glibc/musl but were not
 * yet in b1nix's libc; this file supplies real implementations (no fakes):
 *
 *   __errno_location  — std reads errno through __errno_location() (the
 *                       Linux/musl convention). b1nix's errno is a single
 *                       global int, so returning &errno is correct.
 *   bcmp              — BSD byte-compare; std's core/str search and HashMap
 *                       paths emit bcmp. Identical to memcmp for equality.
 *   getauxval         — std reads the ELF auxiliary vector (page size, etc.).
 *                       The kernel places auxv on the initial stack just past
 *                       the envp NULL terminator; walk it from `environ`.
 *   _Unwind_*         — referenced only by std's backtrace symbolization
 *                       (gimli). The b1nix target builds with panic=abort and
 *                       discards .eh_frame, so unwinding never runs; these are
 *                       inert stubs that exist only to satisfy the link. They
 *                       are never called on a panic=abort target.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <elf.h>
#include <signal.h>
#include <pthread.h>

/* errno is the single global defined in stdlib.c. */
extern int errno;

int *__errno_location(void) {
  return &errno;
}

/* Weak so a port that ships its own bcmp (some do, guarded by !HAVE_BCMP)
 * overrides this without a duplicate-symbol link error. */
__attribute__((weak)) int bcmp(const void *a, const void *b, size_t n) {
  return memcmp(a, b, n);
}

/* environ is set by crt0 to point at the envp array on the initial stack. The
 * auxiliary vector immediately follows the NULL that terminates envp. */
extern char **environ;

unsigned long getauxval(unsigned long type) {
  char **e = environ;
  if (!e)
    return 0;
  /* Advance past the environment string pointers to the NULL terminator. */
  while (*e)
    e++;
  e++; /* step over the NULL: now at the start of the auxv array */

  Elf64_auxv_t *av = (Elf64_auxv_t *)e;
  for (; av->a_type != AT_NULL; av++) {
    if (av->a_type == type)
      return (unsigned long)av->a_un.a_val;
  }
  /* AT_PAGESZ is occasionally absent; std treats 0 as "use the default". */
  return 0;
}

/* M75: run the shared libraries' C++ static constructors before main().
 *
 * b1nix links dynamic executables eagerly in-kernel (no userspace ld.so), and
 * crt0 only walks the *executable's* own __init_array — so a shared library's
 * DT_INIT_ARRAY constructors would never run. For a library like libLLVM.so that
 * is fatal: its 458 constructors register the X86 target and seed the register
 * allocator's cl::opt defaults, and without them clang's backend aborts ("Must
 * use fast register allocator") or crashes in codegen.
 *
 * The kernel collects each library's init_array into a {init_array_va, count}
 * descriptor table (deepest dependency first) and passes it via AT_B1NIX_DSO_INIT
 * (terminated by a zero init_array_va). crt0 calls this once, before the
 * executable's __init_array, so libraries initialize before the program. Each
 * init_array entry is the SysV ABI's void(int, char**, char**). */
#ifndef AT_B1NIX_DSO_INIT
#define AT_B1NIX_DSO_INIT 0x1000
#endif

typedef void (*b1nix_init_fn)(int, char **, char **);

/* Register each loaded shared object's .eh_frame with libgcc's classic FDE
 * registry. The libgcc_s.so DWARF unwinder finds FDEs ONLY through that registry
 * (it was built without the dl_iterate_phdr lookup path), and crt0 registers only
 * the main executable — so without this a C++ exception thrown inside a shared
 * library (e.g. libstdc++.so.6's __cxa_throw) has no FDE for its own throw frame
 * and std::terminate aborts. We walk the kernel's module list via dl_iterate_phdr,
 * skip module 0 (the executable, already registered by crt0), find each library's
 * PT_GNU_EH_FRAME, decode the .eh_frame_hdr's eh_frame_ptr to the .eh_frame start
 * and hand it to __register_frame (the .so carries a zero terminator so the scan
 * stops cleanly). __register_frame is weak: a binary with no libgcc (pure clang C)
 * simply skips this. */
#include <link.h>
#include "syscall.h"
#ifndef PT_GNU_EH_FRAME
#define PT_GNU_EH_FRAME 0x6474e550
#endif

extern void __register_frame(void *) __attribute__((weak));

/* Register every shared object's .eh_frame with libgcc's DWARF unwinder.
 *
 * The kernel records each loaded module's in-process .eh_frame address (located
 * via its section header table) in the dl_iterate_phdr table; we read it with
 * SYS_DL_PHDR_INFO DIRECTLY rather than via dl_iterate_phdr (libgcc_s.so exports
 * its own dl_iterate_phdr stub that returns 0 and, being resolved before
 * libc.so.1, shadows the real one; the raw syscall is an inline instruction, not
 * an interposable symbol, so it is immune). Every shared library past the
 * executable (module 0, registered by crt0) is handed to __register_frame —
 * INCLUDING libgcc_s.so itself, whose .eh_frame the unwinder needs to unwind its
 * own _Unwind_RaiseException frame (libgcc_s.so's frame_dummy never runs: this
 * newlib target has no crti/crtn, so the .so has no DT_INIT). Each .eh_frame ends
 * in the standard zero terminator __register_frame scans to. __register_frame is
 * weak: a pure-clang binary with no libgcc skips this. */
static void b1nix_register_dso_frames(void) {
  if (!(&__register_frame))
    return; /* no libgcc in this process */
  struct b1nix_dl_module mods[16];
  long n = syscall(SYS_DL_PHDR_INFO, (long)(unsigned long)mods,
                   (long)(sizeof(mods) / sizeof(mods[0])), 0, 0, 0);
  if (n > (long)(sizeof(mods) / sizeof(mods[0])))
    n = (long)(sizeof(mods) / sizeof(mods[0]));
  for (long i = 1; i < n; i++) /* module 0 is the executable; crt0 did it */
    if (mods[i].eh_frame_va)
      __register_frame((void *)(uintptr_t)mods[i].eh_frame_va);
}

void __b1nix_run_dso_init(int argc, char **argv, char **envp) {
  /* Register shared-library exception frames. Done before the executable's own
   * __init_array (and before main), so any throw can unwind. */
  b1nix_register_dso_frames();

  unsigned long t = getauxval(AT_B1NIX_DSO_INIT);
  if (!t)
    return;
  unsigned long *desc = (unsigned long *)t;
  for (; desc[0]; desc += 2) {
    b1nix_init_fn *arr = (b1nix_init_fn *)(void *)desc[0];
    unsigned long count = desc[1];
    for (unsigned long i = 0; i < count; i++)
      if (arr[i])
        arr[i](argc, argv, envp);
  }
}

/* __xpg_strerror_r — the XSI-compliant strerror_r (returns int). b1nix's
 * strerror_r already has XSI semantics, so forward to it. (glibc exposes the
 * XSI variant under this internal name; std links it by that name.) */
extern int strerror_r(int errnum, char *buf, size_t buflen);

int __xpg_strerror_r(int errnum, char *buf, size_t buflen) {
  return strerror_r(errnum, buf, buflen);
}

/* pause() — suspend until a signal is delivered. Implemented via sigsuspend
 * with the current mask unchanged (the standard libc-less approach). Always
 * returns -1 with errno=EINTR once a handler runs, per POSIX. */
int pause(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigprocmask(SIG_BLOCK, NULL, &mask); /* fetch the current mask */
  sigsuspend(&mask);
  errno = EINTR;
  return -1;
}

/* Per-thread stack guard size. b1nix does not place per-thread guard pages, so
 * the guard size is 0; report that honestly. std's stack-overflow handler
 * treats 0 as "no guard page" and degrades gracefully. */
int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize) {
  (void)attr;
  if (!guardsize)
    return EINVAL;
  *guardsize = 0;
  return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize) {
  (void)attr;
  (void)guardsize;
  return 0; /* accepted but not honored — no per-thread guard pages */
}

/* The `_Unwind_*` ABI is provided in full by the real gcc unwinder
 * (libgcc_eh.a, aliased as -lunwind for rust's musl+crt-static link). Earlier
 * inert abort()-stubs here defined only _Unwind_GetIP/_Unwind_Backtrace, which
 * collided with libgcc_eh's strong definitions ("duplicate symbol"). Dropped:
 * the real unwinder is correct and complete, so no stubs are needed. */
