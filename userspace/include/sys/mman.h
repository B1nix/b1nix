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

#define MAP_FAILED      ((void *)-1)

#define MFD_CLOEXEC     0x0001U

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t len, int prot);
int memfd_create(const char *name, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif
