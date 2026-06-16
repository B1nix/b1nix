#ifndef B1NIX_U_ICONV_H
#define B1NIX_U_ICONV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* iconv for B1NIX — character-set conversion between the common encodings used
 * by ported software: UTF-8, UTF-16 (LE/BE/host), UCS-4/UTF-32 (== wchar_t),
 * ISO-8859-1 (Latin-1) and US-ASCII. Conversions pivot through Unicode code
 * points. Encoding names are matched case-insensitively, ignoring '-'/'_'/'.'
 * separators (so "UTF-8", "utf8" and "UTF_8" are equivalent).
 *
 * Semantics follow POSIX: iconv() returns the number of characters converted
 * in a non-reversible way, or (size_t)-1 with errno set to EILSEQ (invalid
 * input sequence), EINVAL (incomplete trailing sequence) or E2BIG (output
 * buffer full). iconv_open() returns (iconv_t)-1 with errno EINVAL for an
 * unsupported conversion. */

typedef void *iconv_t;

iconv_t iconv_open(const char *tocode, const char *fromcode);
size_t  iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
              char **outbuf, size_t *outbytesleft);
int     iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif

#endif
