/* iconv for b1nix — see userspace/include/iconv.h for the contract.
 *
 * Every conversion pivots through Unicode code points (uint32_t): the input is
 * decoded one code point at a time in the source encoding, then re-encoded in
 * the destination encoding. This keeps each encoding's decode/encode logic
 * independent and lets any supported pair interoperate. */

#include <iconv.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum enc {
  ENC_UTF8,
  ENC_UTF16LE,
  ENC_UTF16BE,
  ENC_UCS4,      /* host-endian UTF-32 / wchar_t */
  ENC_LATIN1,
  ENC_ASCII,
};

struct iconv_ctx {
  enum enc from;
  enum enc to;
};

/* Canonicalize an encoding name: upper-case, dropping '-', '_', '.', and
 * spaces, into dst (truncated to dstlen-1). */
static void canon(const char *s, char *dst, size_t dstlen) {
  size_t j = 0;
  for (; s && *s && j + 1 < dstlen; s++) {
    char c = *s;
    if (c == '-' || c == '_' || c == '.' || c == ' ')
      continue;
    if (c >= 'a' && c <= 'z')
      c = (char)(c - 'a' + 'A');
    dst[j++] = c;
  }
  dst[j] = '\0';
}

static int parse_enc(const char *name, enum enc *out) {
  char c[32];
  canon(name, c, sizeof(c));
  if (!strcmp(c, "UTF8")) { *out = ENC_UTF8; return 0; }
  if (!strcmp(c, "UTF16") || !strcmp(c, "UTF16LE") || !strcmp(c, "UCS2") ||
      !strcmp(c, "UCS2LE")) { *out = ENC_UTF16LE; return 0; }
  if (!strcmp(c, "UTF16BE") || !strcmp(c, "UCS2BE")) { *out = ENC_UTF16BE; return 0; }
  if (!strcmp(c, "UCS4") || !strcmp(c, "UTF32") || !strcmp(c, "UCS4LE") ||
      !strcmp(c, "UTF32LE") || !strcmp(c, "WCHART") || !strcmp(c, "INTERNAL")) {
    *out = ENC_UCS4; return 0;
  }
  if (!strcmp(c, "ISO88591") || !strcmp(c, "LATIN1") || !strcmp(c, "L1") ||
      !strcmp(c, "8859") || !strcmp(c, "ISO8859") || !strcmp(c, "CP819")) {
    *out = ENC_LATIN1; return 0;
  }
  if (!strcmp(c, "ASCII") || !strcmp(c, "USASCII") || !strcmp(c, "ANSIX3.4") ||
      !strcmp(c, "ANSIX34") || !strcmp(c, "646") || !strcmp(c, "ISO646US")) {
    *out = ENC_ASCII; return 0;
  }
  return -1;
}

iconv_t iconv_open(const char *tocode, const char *fromcode) {
  enum enc from, to;
  if (parse_enc(fromcode, &from) != 0 || parse_enc(tocode, &to) != 0) {
    errno = EINVAL;
    return (iconv_t)-1;
  }
  struct iconv_ctx *ctx = (struct iconv_ctx *)malloc(sizeof(*ctx));
  if (!ctx) {
    errno = ENOMEM;
    return (iconv_t)-1;
  }
  ctx->from = from;
  ctx->to = to;
  return (iconv_t)ctx;
}

int iconv_close(iconv_t cd) {
  if (cd == (iconv_t)-1 || !cd) {
    errno = EBADF;
    return -1;
  }
  free(cd);
  return 0;
}

/* Decode one code point from `in` (with `left` bytes available). On success
 * stores the code point in *cp, the number of bytes consumed in *used, and
 * returns 0. Returns EILSEQ for an invalid sequence or EINVAL for a valid but
 * truncated trailing sequence. */
static int decode_cp(enum enc enc, const unsigned char *in, size_t left,
                     uint32_t *cp, size_t *used) {
  switch (enc) {
  case ENC_ASCII:
    if (in[0] >= 0x80) return EILSEQ;
    *cp = in[0]; *used = 1; return 0;
  case ENC_LATIN1:
    *cp = in[0]; *used = 1; return 0;
  case ENC_UCS4:
    if (left < 4) return EINVAL;
    *cp = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
          ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if (*cp > 0x10FFFF) return EILSEQ;
    *used = 4; return 0;
  case ENC_UTF16LE:
  case ENC_UTF16BE: {
    if (left < 2) return EINVAL;
    uint32_t u0 = (enc == ENC_UTF16LE)
                      ? ((uint32_t)in[0] | ((uint32_t)in[1] << 8))
                      : (((uint32_t)in[0] << 8) | in[1]);
    if (u0 < 0xD800 || u0 > 0xDFFF) { *cp = u0; *used = 2; return 0; }
    if (u0 >= 0xDC00) return EILSEQ; /* unpaired low surrogate */
    if (left < 4) return EINVAL;
    uint32_t u1 = (enc == ENC_UTF16LE)
                      ? ((uint32_t)in[2] | ((uint32_t)in[3] << 8))
                      : (((uint32_t)in[2] << 8) | in[3]);
    if (u1 < 0xDC00 || u1 > 0xDFFF) return EILSEQ;
    *cp = 0x10000 + ((u0 - 0xD800) << 10) + (u1 - 0xDC00);
    *used = 4; return 0;
  }
  case ENC_UTF8:
  default: {
    unsigned char b0 = in[0];
    if (b0 < 0x80) { *cp = b0; *used = 1; return 0; }
    int n;
    uint32_t c;
    if ((b0 & 0xE0) == 0xC0) { n = 2; c = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { n = 3; c = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { n = 4; c = b0 & 0x07; }
    else return EILSEQ;
    if (left < (size_t)n) return EINVAL;
    for (int i = 1; i < n; i++) {
      if ((in[i] & 0xC0) != 0x80) return EILSEQ;
      c = (c << 6) | (in[i] & 0x3F);
    }
    /* Reject overlong encodings and out-of-range code points. */
    static const uint32_t min_cp[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (c < min_cp[n] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
      return EILSEQ;
    *cp = c; *used = n; return 0;
  }
  }
}

/* Encode one code point into `out` (with `room` bytes available). On success
 * stores the byte count in *used and returns 0. Returns E2BIG if the output is
 * too small, or EILSEQ if `cp` cannot be represented in the target encoding. */
static int encode_cp(enum enc enc, uint32_t cp, unsigned char *out, size_t room,
                     size_t *used) {
  switch (enc) {
  case ENC_ASCII:
    if (cp > 0x7F) return EILSEQ;
    if (room < 1) return E2BIG;
    out[0] = (unsigned char)cp; *used = 1; return 0;
  case ENC_LATIN1:
    if (cp > 0xFF) return EILSEQ;
    if (room < 1) return E2BIG;
    out[0] = (unsigned char)cp; *used = 1; return 0;
  case ENC_UCS4:
    if (room < 4) return E2BIG;
    out[0] = (unsigned char)(cp & 0xFF);
    out[1] = (unsigned char)((cp >> 8) & 0xFF);
    out[2] = (unsigned char)((cp >> 16) & 0xFF);
    out[3] = (unsigned char)((cp >> 24) & 0xFF);
    *used = 4; return 0;
  case ENC_UTF16LE:
  case ENC_UTF16BE: {
    if (cp < 0x10000) {
      if (cp >= 0xD800 && cp <= 0xDFFF) return EILSEQ;
      if (room < 2) return E2BIG;
      if (enc == ENC_UTF16LE) { out[0] = cp & 0xFF; out[1] = (cp >> 8) & 0xFF; }
      else                    { out[0] = (cp >> 8) & 0xFF; out[1] = cp & 0xFF; }
      *used = 2; return 0;
    }
    if (cp > 0x10FFFF) return EILSEQ;
    if (room < 4) return E2BIG;
    uint32_t v = cp - 0x10000;
    uint32_t hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
    if (enc == ENC_UTF16LE) {
      out[0] = hi & 0xFF; out[1] = (hi >> 8) & 0xFF;
      out[2] = lo & 0xFF; out[3] = (lo >> 8) & 0xFF;
    } else {
      out[0] = (hi >> 8) & 0xFF; out[1] = hi & 0xFF;
      out[2] = (lo >> 8) & 0xFF; out[3] = lo & 0xFF;
    }
    *used = 4; return 0;
  }
  case ENC_UTF8:
  default:
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return EILSEQ;
    if (cp < 0x80) {
      if (room < 1) return E2BIG;
      out[0] = (unsigned char)cp; *used = 1; return 0;
    }
    if (cp < 0x800) {
      if (room < 2) return E2BIG;
      out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F);
      *used = 2; return 0;
    }
    if (cp < 0x10000) {
      if (room < 3) return E2BIG;
      out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F);
      out[2] = 0x80 | (cp & 0x3F); *used = 3; return 0;
    }
    if (room < 4) return E2BIG;
    out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F);
    *used = 4; return 0;
  }
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft) {
  struct iconv_ctx *ctx = (struct iconv_ctx *)cd;
  if (cd == (iconv_t)-1 || !ctx) { errno = EBADF; return (size_t)-1; }

  /* NULL inbuf (or *inbuf) requests a state reset. Our conversions are
   * stateless, so there is nothing to flush — just succeed. */
  if (!inbuf || !*inbuf || !inbytesleft)
    return 0;

  size_t nonrev = 0;
  while (*inbytesleft > 0) {
    uint32_t cp;
    size_t used_in, used_out;
    int rc = decode_cp(ctx->from, (const unsigned char *)*inbuf, *inbytesleft,
                       &cp, &used_in);
    if (rc != 0) { errno = rc; return (size_t)-1; }

    rc = encode_cp(ctx->to, cp, (unsigned char *)*outbuf, *outbytesleft,
                   &used_out);
    if (rc != 0) { errno = rc; return (size_t)-1; }

    *inbuf += used_in;
    *inbytesleft -= used_in;
    *outbuf += used_out;
    *outbytesleft -= used_out;
  }
  return nonrev;
}
