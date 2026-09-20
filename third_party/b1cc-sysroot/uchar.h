/* C11 <uchar.h> — Unicode utilities (char16_t / char32_t).
 *
 * In C++ char16_t and char32_t are language keywords, so the typedefs are
 * guarded for C only. The conversion functions wrap b1nix's UTF-8 multibyte
 * machinery (mbrtowc/wcrtomb): char32_t is UCS-4 (== wchar_t here), char16_t is
 * UTF-16 with surrogate-pair handling. Encoding is UTF-8 only (C locale).
 */
#ifndef _UCHAR_H
#define _UCHAR_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>   /* mbstate_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __cplusplus
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif

size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps);
size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps);
size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps);
size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps);

#ifdef __cplusplus
}
#endif

#endif /* _UCHAR_H */
