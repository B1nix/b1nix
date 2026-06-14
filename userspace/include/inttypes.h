#ifndef B1NIX_U_INTTYPES_H
#define B1NIX_U_INTTYPES_H

#include <stdint.h>

#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIu64 "llu"
#define PRIo64 "llo"
#define PRIx64 "llx"
#define PRIX64 "llX"

#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIo32 "o"
#define PRIx32 "x"
#define PRIX32 "X"

#define PRId16 "d"
#define PRIi16 "i"
#define PRIu16 "u"
#define PRIo16 "o"
#define PRIx16 "x"
#define PRIX16 "X"

#define PRId8 "d"
#define PRIi8 "i"
#define PRIu8 "u"
#define PRIo8 "o"
#define PRIx8 "x"
#define PRIX8 "X"

#define SCNd64 "lld"
#define SCNi64 "lli"
#define SCNu64 "llu"
#define SCNo64 "llo"
#define SCNx64 "llx"

#define SCNd32 "d"
#define SCNi32 "i"
#define SCNu32 "u"
#define SCNo32 "o"
#define SCNx32 "x"

#define SCNd16 "hd"
#define SCNi16 "hi"
#define SCNu16 "hu"
#define SCNo16 "ho"
#define SCNx16 "hx"


/* intmax_t / uintmax_t format macros (intmax_t == long long here). */
#define PRIdMAX "lld"
#define PRIiMAX "lli"
#define PRIoMAX "llo"
#define PRIuMAX "llu"
#define PRIxMAX "llx"
#define PRIXMAX "llX"
#define SCNdMAX "lld"
#define SCNiMAX "lli"
#define SCNoMAX "llo"
#define SCNuMAX "llu"
#define SCNxMAX "llx"


/* uintptr_t / intptr_t format macros (pointer width is arch-dependent). */
#if defined(__x86_64__) || defined(__LP64__)
#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIoPTR "lo"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"
#define SCNdPTR "ld"
#define SCNiPTR "li"
#define SCNuPTR "lu"
#define SCNxPTR "lx"
#else
#define PRIdPTR "d"
#define PRIiPTR "i"
#define PRIoPTR "o"
#define PRIuPTR "u"
#define PRIxPTR "x"
#define PRIXPTR "X"
#define SCNdPTR "d"
#define SCNiPTR "i"
#define SCNuPTR "u"
#define SCNxPTR "x"
#endif

#endif
