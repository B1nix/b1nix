#ifndef B1NIX_U_SYS_MMAN_H
#define B1NIX_U_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

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
/* BSD alias used by Chromium base/ and others. */
#define MAP_ANON        MAP_ANONYMOUS

#define MAP_FAILED      ((void *)-1)

/* msync() flags (Linux ABI numbers). */
#define MS_ASYNC        1
#define MS_INVALIDATE   2
#define MS_SYNC         4

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
#define MADV_DONTDUMP   16
#define MADV_DODUMP     17

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t length, int advice);
int msync(void *addr, size_t length, int flags);
int memfd_create(const char *name, unsigned int flags);

/* POSIX named shared memory (glibc declares these in <sys/mman.h>). b1nix has
 * no /dev/shm tmpfs namespace, so shm_open() is implemented on top of the
 * existing anonymous-shared-memory primitive memfd_create(): it returns a real
 * fd that backs ftruncate() + mmap(MAP_SHARED) exactly as POSIX requires. The
 * `name` is therefore not a persistent namespace key — each shm_open() yields a
 * fresh unnamed object — so shm_unlink() is a no-op success (the object is
 * reclaimed when the last fd/mmap is dropped, like an O_TMPFILE/already-unlinked
 * file). This is sufficient for the only in-tree caller (LLVM's Orc JIT
 * shared-memory mapper), which creates, ftruncates, mmaps and closes the fd in
 * one process. The O_EXCL/O_CREAT oflags and mode are honored only insofar as
 * memfd permits; a name-collision cannot occur because there is no namespace. */
int shm_open(const char *name, int oflag, mode_t mode);
int shm_unlink(const char *name);

/* b1nix has no mremap syscall; the wrapper fails (MAP_FAILED/ENOMEM) so callers
 * fall back to munmap+mmap. Declared for source compatibility (V8 perf-jit). */
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2
void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif
