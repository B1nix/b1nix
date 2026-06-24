#ifndef _PTRAUTH_H
#define _PTRAUTH_H

/* <ptrauth.h>: ARM64 pointer authentication intrinsics. On x86 (the only
 * b1nix arch) pointer auth does not exist, so these are no-ops. Chromium's
 * base/profiler includes this header unconditionally but only uses the
 * intrinsics under ARCH_CPU_ARM64, so empty no-op macros are correct here.
 * Added for the Chromium port (M60-62). */

#define ptrauth_strip(__value, __key) (__value)
#define ptrauth_sign_unauthenticated(__value, __key, __data) (__value)
#define ptrauth_auth_and_resign(__value, __old_key, __old_data, __new_key, \
                                __new_data)                                \
  (__value)
#define ptrauth_auth_data(__value, __key, __data) (__value)
#define ptrauth_string_discriminator(__string) (0)
#define ptrauth_blend_discriminator(__pointer, __integer) (0)
#define ptrauth_sign_constant(__value, __key, __data) (__value)
#define ptrauth_type_discriminator(__type) (0)

#endif /* _PTRAUTH_H */
