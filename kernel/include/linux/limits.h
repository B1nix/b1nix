/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_LIMITS_H
#define LKPI_LINUX_LIMITS_H
#define U8_MAX   ((u8)~0u)
#define S8_MAX   ((s8)(U8_MAX >> 1))
#define S8_MIN   ((s8)(-S8_MAX - 1))
#define U16_MAX  ((u16)~0u)
#define S16_MAX  ((s16)(U16_MAX >> 1))
#define S16_MIN  ((s16)(-S16_MAX - 1))
#define U32_MAX  ((u32)~0u)
#define S32_MAX  ((s32)(U32_MAX >> 1))
#define S32_MIN  ((s32)(-S32_MAX - 1))
#define U64_MAX  ((u64)~0ull)
#define S64_MAX  ((s64)(U64_MAX >> 1))
#define S64_MIN  ((s64)(-S64_MAX - 1))
#define INT_MAX  S32_MAX
#define INT_MIN  S32_MIN
#define UINT_MAX U32_MAX
#define LONG_MAX S64_MAX
#define LONG_MIN S64_MIN
#define ULONG_MAX ((unsigned long)~0ul)
#ifndef SIZE_MAX
#define SIZE_MAX  ULONG_MAX
#endif
#define PATH_MAX  4096
#define NAME_MAX  255
#define PAGE_SIZE_LINUX 4096

#define USHRT_MAX U16_MAX
#define SHRT_MAX  S16_MAX
#define SHRT_MIN  S16_MIN
#define UCHAR_MAX U8_MAX
#define CHAR_MAX  S8_MAX

#endif
