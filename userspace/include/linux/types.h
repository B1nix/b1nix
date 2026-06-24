#ifndef B1NIX_U_LINUX_TYPES_H
#define B1NIX_U_LINUX_TYPES_H

#include <stdint.h>

typedef uint8_t  __u8;
typedef int8_t   __s8;
typedef uint16_t __u16;
typedef int16_t  __s16;
typedef uint32_t __u32;
typedef int32_t  __s32;
typedef uint64_t __u64;
typedef int64_t  __s64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint16_t __sum16;
typedef unsigned short __kernel_sa_family_t;

/* __kernel_* integer types (Linux UAPI). Added for the Chromium port (M60-62):
 * libdrm's <drm/drm.h> uses them. Sizes match the x86_64 LP64 ABI. */
#include <stddef.h>
typedef size_t          __kernel_size_t;
typedef long            __kernel_ssize_t;
typedef long            __kernel_long_t;
typedef unsigned long   __kernel_ulong_t;
typedef int             __kernel_pid_t;
typedef unsigned int    __kernel_uid32_t;
typedef unsigned int    __kernel_gid32_t;
typedef long long       __kernel_loff_t;
typedef long long       __kernel_time64_t;
typedef long            __kernel_off_t;

#endif
