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
