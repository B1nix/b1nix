#ifndef B1NIX_U_STDINT_H
#define B1NIX_U_STDINT_H

typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

typedef long intptr_t;
typedef unsigned long uintptr_t;

typedef long long intmax_t;
typedef unsigned long long uintmax_t;

#define INT8_C(c)  c
#define UINT8_C(c) c
#define INT16_C(c)  c
#define UINT16_C(c) c
#define INT32_C(c)  c
#define UINT32_C(c) c
#define INT64_C(c)  c ## LL
#define UINT64_C(c) c ## ULL

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255

#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535

#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295U

#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT64_MAX  9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL

#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#define INTPTR_MIN  (-9223372036854775807L - 1)
#define INTPTR_MAX  9223372036854775807L
#define UINTPTR_MAX 18446744073709551615UL

#define SIZE_MAX  18446744073709551615UL
#define SSIZE_MAX 9223372036854775807L

#endif
