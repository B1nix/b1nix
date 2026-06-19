#ifndef B1NIX_U_MALLOC_H
#define B1NIX_U_MALLOC_H

#include <stdlib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t malloc_usable_size(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
