#ifndef B1NIX_U_MALLOC_H
#define B1NIX_U_MALLOC_H

#include <stdlib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t malloc_usable_size(void *ptr);

/* struct mallinfo (added for the Chromium port, M60-62). The classic glibc
 * layout; partition_alloc's allocator_shim defines mallinfo()/mallinfo2() for
 * IS_LINUX. b1nix does not maintain these statistics natively, but the type must
 * exist for the shim to compile and provide them. */
struct mallinfo {
  int arena;
  int ordblks;
  int smblks;
  int hblks;
  int hblkhd;
  int usmblks;
  int fsmblks;
  int uordblks;
  int fordblks;
  int keepcost;
};
struct mallinfo mallinfo(void);

struct mallinfo2 {
  size_t arena;
  size_t ordblks;
  size_t smblks;
  size_t hblks;
  size_t hblkhd;
  size_t usmblks;
  size_t fsmblks;
  size_t uordblks;
  size_t fordblks;
  size_t keepcost;
};
struct mallinfo2 mallinfo2(void);

#ifdef __cplusplus
}
#endif

#endif
