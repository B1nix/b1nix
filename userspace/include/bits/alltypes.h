/* bits/alltypes.h — musl-style on-demand type provider.
 *
 * b1nix's libc is musl-like, so LLVM libc++ is built with _LIBCPP_HAS_MUSL_LIBC
 * (it gives us the default rune table and the C-locale-only *_l shims). The musl
 * code path obtains mbstate_t the musl way:
 *
 *     #define __NEED_mbstate_t
 *     #include <bits/alltypes.h>
 *
 * Following musl's protocol, this header is intentionally NOT include-guarded:
 * it defines only the type(s) selected by the currently-set __NEED_* macros, so
 * it may be re-included with a different __NEED_* set. Each type sits behind its
 * own one-shot guard that matches the canonical guard used by the b1nix libc
 * headers (e.g. <wchar.h>), with an identical layout — so pulling a type in
 * through this header and later including the full <wchar.h> in the same
 * translation unit never produces a conflicting redefinition.
 *
 * Only the types LLVM libc++ actually requests are provided today (mbstate_t);
 * add further __NEED_* blocks here as new consumers appear, always mirroring the
 * canonical b1nix definition + guard. */

#if defined(__NEED_mbstate_t) && !defined(B1NIX_MBSTATE_T_DEFINED)
#define B1NIX_MBSTATE_T_DEFINED
typedef struct __mbstate_t {
	unsigned int __count;
	unsigned int __value;
} mbstate_t;
#endif
