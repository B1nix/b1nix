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

/* 8-bit scanf macros use the "hh" length modifier (uuid.cc scans uint8_t). */
#define SCNd8 "hhd"
#define SCNi8 "hhi"
#define SCNu8 "hhu"
#define SCNo8 "hho"
#define SCNx8 "hhx"


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


/* int_leastN_t and int_fastN_t are identical to intN_t on b1nix (LP64), so
 * the LEAST and FAST format macros alias the base-width PRI and SCN macros. */
#define PRIdLEAST8 PRId8
#define PRIiLEAST8 PRIi8
#define PRIuLEAST8 PRIu8
#define PRIoLEAST8 PRIo8
#define PRIxLEAST8 PRIx8
#define PRIXLEAST8 PRIX8
#define PRIdLEAST16 PRId16
#define PRIiLEAST16 PRIi16
#define PRIuLEAST16 PRIu16
#define PRIoLEAST16 PRIo16
#define PRIxLEAST16 PRIx16
#define PRIXLEAST16 PRIX16
#define PRIdLEAST32 PRId32
#define PRIiLEAST32 PRIi32
#define PRIuLEAST32 PRIu32
#define PRIoLEAST32 PRIo32
#define PRIxLEAST32 PRIx32
#define PRIXLEAST32 PRIX32
#define PRIdLEAST64 PRId64
#define PRIiLEAST64 PRIi64
#define PRIuLEAST64 PRIu64
#define PRIoLEAST64 PRIo64
#define PRIxLEAST64 PRIx64
#define PRIXLEAST64 PRIX64
#define PRIdFAST8 PRId8
#define PRIiFAST8 PRIi8
#define PRIuFAST8 PRIu8
#define PRIoFAST8 PRIo8
#define PRIxFAST8 PRIx8
#define PRIXFAST8 PRIX8
#define PRIdFAST16 PRId16
#define PRIiFAST16 PRIi16
#define PRIuFAST16 PRIu16
#define PRIoFAST16 PRIo16
#define PRIxFAST16 PRIx16
#define PRIXFAST16 PRIX16
#define PRIdFAST32 PRId32
#define PRIiFAST32 PRIi32
#define PRIuFAST32 PRIu32
#define PRIoFAST32 PRIo32
#define PRIxFAST32 PRIx32
#define PRIXFAST32 PRIX32
#define PRIdFAST64 PRId64
#define PRIiFAST64 PRIi64
#define PRIuFAST64 PRIu64
#define PRIoFAST64 PRIo64
#define PRIxFAST64 PRIx64
#define PRIXFAST64 PRIX64
#define SCNdLEAST8 SCNd8
#define SCNiLEAST8 SCNi8
#define SCNuLEAST8 SCNu8
#define SCNoLEAST8 SCNo8
#define SCNxLEAST8 SCNx8
#define SCNdLEAST16 SCNd16
#define SCNiLEAST16 SCNi16
#define SCNuLEAST16 SCNu16
#define SCNoLEAST16 SCNo16
#define SCNxLEAST16 SCNx16
#define SCNdLEAST32 SCNd32
#define SCNiLEAST32 SCNi32
#define SCNuLEAST32 SCNu32
#define SCNoLEAST32 SCNo32
#define SCNxLEAST32 SCNx32
#define SCNdLEAST64 SCNd64
#define SCNiLEAST64 SCNi64
#define SCNuLEAST64 SCNu64
#define SCNoLEAST64 SCNo64
#define SCNxLEAST64 SCNx64
#define SCNdFAST8 SCNd8
#define SCNiFAST8 SCNi8
#define SCNuFAST8 SCNu8
#define SCNoFAST8 SCNo8
#define SCNxFAST8 SCNx8
#define SCNdFAST16 SCNd16
#define SCNiFAST16 SCNi16
#define SCNuFAST16 SCNu16
#define SCNoFAST16 SCNo16
#define SCNxFAST16 SCNx16
#define SCNdFAST32 SCNd32
#define SCNiFAST32 SCNi32
#define SCNuFAST32 SCNu32
#define SCNoFAST32 SCNo32
#define SCNxFAST32 SCNx32
#define SCNdFAST64 SCNd64
#define SCNiFAST64 SCNi64
#define SCNuFAST64 SCNu64
#define SCNoFAST64 SCNo64
#define SCNxFAST64 SCNx64

#endif
