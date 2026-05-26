#ifndef B1NIX_STDDEF_H
#define B1NIX_STDDEF_H

#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif

typedef unsigned long size_t;
typedef long ssize_t;
typedef long ptrdiff_t;

#if __STDC_VERSION__ >= 201112L || defined(__cplusplus)
typedef struct {
  long long __max_align_ll __attribute__((__aligned__(__alignof__(long long))));
  long double __max_align_ld __attribute__((__aligned__(__alignof__(long double))));
} max_align_t;
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
