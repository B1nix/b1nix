#ifndef B1NIX_U_LIMITS_H
#define B1NIX_U_LIMITS_H
#ifndef NZERO
#define NZERO 20  /* default nice value offset */
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 64  /* max gethostname() length (POSIX/Linux value) */
#endif

#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif

#ifndef SCHAR_MIN
#define SCHAR_MIN (-128)
#endif
#ifndef SCHAR_MAX
#define SCHAR_MAX 127
#endif
#ifndef UCHAR_MAX
#define UCHAR_MAX 255
#endif

#ifndef CHAR_MIN
#define CHAR_MIN SCHAR_MIN
#endif
#ifndef CHAR_MAX
#define CHAR_MAX SCHAR_MAX
#endif

#ifndef SHRT_MIN
#define SHRT_MIN (-32768)
#endif
#ifndef SHRT_MAX
#define SHRT_MAX 32767
#endif
#ifndef USHRT_MAX
#define USHRT_MAX 65535
#endif

#ifndef INT_MIN
#define INT_MIN (-2147483647 - 1)
#endif
#ifndef INT_MAX
#define INT_MAX 2147483647
#endif
#ifndef UINT_MAX
#define UINT_MAX 4294967295U
#endif

#if defined(__x86_64__) || defined(__LP64__) || defined(__aarch64__) || defined(__arm64__)
#ifndef LONG_MIN
#define LONG_MIN (-9223372036854775807L - 1)
#endif
#ifndef LONG_MAX
#define LONG_MAX 9223372036854775807L
#endif
#ifndef ULONG_MAX
#define ULONG_MAX 18446744073709551615UL
#endif
#else
#ifndef LONG_MIN
#define LONG_MIN (-2147483647L - 1)
#endif
#ifndef LONG_MAX
#define LONG_MAX 2147483647L
#endif
#ifndef ULONG_MAX
#define ULONG_MAX 4294967295UL
#endif
#endif

#ifndef LLONG_MIN
#define LLONG_MIN (-9223372036854775807LL - 1)
#endif
#ifndef LLONG_MAX
#define LLONG_MAX 9223372036854775807LL
#endif
#ifndef ULLONG_MAX
#define ULLONG_MAX 18446744073709551615ULL
#endif

#ifndef MB_LEN_MAX
#define MB_LEN_MAX 4
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef _POSIX_ARG_MAX
#define _POSIX_ARG_MAX 4096
#endif
#ifndef ARG_MAX
#define ARG_MAX 131072
#endif
#ifndef LINK_MAX
#define LINK_MAX 127
#endif
#ifndef OPEN_MAX
#define OPEN_MAX 256
#endif
#ifndef PIPE_BUF
#define PIPE_BUF 4096
#endif

#endif
