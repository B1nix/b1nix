#ifndef B1NIX_U_SYS_MMAN_H
#define B1NIX_U_SYS_MMAN_H

#include <stddef.h>

#define PROT_NONE       0x00
#define PROT_READ       0x01
#define PROT_WRITE      0x02
#define PROT_EXEC       0x04

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
/* MAP_NORESERVE: lazy-commit (the only commit model b1nix has for anonymous
 * mappings). Accepted by the kernel, selects commit-on-touch. */
#define MAP_NORESERVE   0x4000

#define MAP_FAILED      ((void *)-1)

#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U

/* madvise() advice values (Linux ABI numbers). */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8
/* Advisory hints b1nix accepts but does not act on (legal POSIX no-op).
 * V8's platform-posix.cc tags allocations with DONTFORK/HUGEPAGE. */
#define MADV_DONTFORK   10
#define MADV_DOFORK     11
#define MADV_HUGEPAGE   14
#define MADV_NOHUGEPAGE 15

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t length, int advice);
int memfd_create(const char *name, unsigned int flags);

/* b1nix has no mremap syscall; the wrapper fails (MAP_FAILED/ENOMEM) so callers
 * fall back to munmap+mmap. Declared for source compatibility (V8 perf-jit). */
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2
void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif
