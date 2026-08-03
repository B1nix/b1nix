/* M53 libparserutils smoke: prove the ported NetSurf input/charset library
 * works on b1nix. libparserutils sits under libhubbub/libcss/libdom — every
 * byte of HTML/CSS the browser parses flows through its input streams and its
 * bundled charset codecs (built -DWITHOUT_ICONV_FILTER, no system iconv).
 *
 * Nothing is faked; each marker verifies real behaviour:
 *  - UTF-8 encode/decode round-trips ASCII, 2-, 3- and 4-byte codepoints with
 *    the correct byte lengths,
 *  - utf8_length counts characters (not bytes) in a mixed-width string,
 *  - the charset-alias (MIB enum) table resolves "UTF-8" case-insensitively and
 *    classifies Unicode vs legacy encodings,
 *  - a real ISO-8859-1 codec decodes a high byte to the right UCS-4 codepoint
 *    and a UTF-8 codec encodes UCS-4 back to bytes.
 * Markers (M53-PARSERUTILS: ...) consumed by smoke.sh. */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <parserutils/charset/utf8.h>
#include <parserutils/charset/codec.h>
#include <parserutils/charset/mibenum.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

/* libparserutils codecs exchange UCS-4 in BIG-ENDIAN byte order (its codecs
 * call endian_host_to_big on decode output and endian_big_to_host on encode
 * input), independent of the host. These helpers read/write that ABI. */
static uint32_t be32_read(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void be32_write(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* One UTF-8 round-trip: encode `cp` to bytes, check the byte length, then
 * decode it straight back to the same codepoint. Returns 0 on success. */
static int roundtrip(uint32_t cp, size_t expect_len) {
  uint8_t buf[8];
  uint8_t *p = buf;
  size_t len = sizeof(buf);
  if (parserutils_charset_utf8_from_ucs4(cp, &p, &len) != PARSERUTILS_OK)
    return 1;
  size_t enc = sizeof(buf) - len; /* bytes written */
  if (enc != expect_len)
    return 1;

  uint32_t back = 0;
  size_t clen = 0;
  if (parserutils_charset_utf8_to_ucs4(buf, enc, &back, &clen) != PARSERUTILS_OK)
    return 1;
  if (back != cp || clen != enc)
    return 1;
  return 0;
}

int main(void) {
  emit("M53-PARSERUTILS: start\n");

  /* ── UTF-8 round-trip across all four encoded widths ── */
  if (roundtrip(0x41, 1) ||      /* 'A'            */
      roundtrip(0x00E9, 2) ||    /* 'é'            */
      roundtrip(0x20AC, 3) ||    /* '€'            */
      roundtrip(0x1F600, 4)) {   /* '😀'           */
    emit("M53-PARSERUTILS: fail utf8-roundtrip\n");
    return 1;
  }
  emit("M53-PARSERUTILS: ok utf8-roundtrip\n");

  /* ── utf8_length counts characters, not bytes ── */
  /* "A" + "é"(2) + "€"(3) + "😀"(4) = 4 chars, 10 bytes */
  const uint8_t mixed[] = {0x41, 0xC3, 0xA9, 0xE2, 0x82, 0xAC,
                           0xF0, 0x9F, 0x98, 0x80};
  size_t nchars = 0;
  if (parserutils_charset_utf8_length(mixed, sizeof(mixed), &nchars) !=
          PARSERUTILS_OK ||
      nchars != 4) {
    emit("M53-PARSERUTILS: fail utf8-length\n");
    return 1;
  }
  emit("M53-PARSERUTILS: ok utf8-length\n");

  /* ── charset alias / MIB-enum table ── */
  uint16_t m_lower = parserutils_charset_mibenum_from_name("utf-8", 5);
  uint16_t m_upper = parserutils_charset_mibenum_from_name("UTF-8", 5);
  uint16_t m_8859 = parserutils_charset_mibenum_from_name("ISO-8859-1", 10);
  if (m_lower == 0 || m_lower != m_upper || m_8859 == 0 || m_8859 == m_lower) {
    emit("M53-PARSERUTILS: fail mibenum\n");
    return 1;
  }
  if (!parserutils_charset_mibenum_is_unicode(m_lower) ||
      parserutils_charset_mibenum_is_unicode(m_8859)) {
    emit("M53-PARSERUTILS: fail mibenum-unicode\n");
    return 1;
  }
  const char *name = parserutils_charset_mibenum_to_name(m_lower);
  if (name == NULL) {
    emit("M53-PARSERUTILS: fail mibenum-name\n");
    return 1;
  }
  emit("M53-PARSERUTILS: ok mibenum\n");

  /* ── A real ISO-8859-1 codec: decode high byte 0xE9 to UCS-4 'é' (0xE9) ── */
  parserutils_charset_codec *dec = NULL;
  if (parserutils_charset_codec_create("ISO-8859-1", &dec) != PARSERUTILS_OK ||
      dec == NULL) {
    emit("M53-PARSERUTILS: fail codec-create\n");
    return 1;
  }
  {
    const uint8_t in[] = {0xE9};
    const uint8_t *sp = in;
    size_t slen = sizeof(in);
    uint8_t outbuf[8] = {0};
    uint8_t *dp = outbuf;
    size_t dlen = sizeof(outbuf);
    if (parserutils_charset_codec_decode(dec, &sp, &slen, &dp, &dlen) !=
            PARSERUTILS_OK ||
        be32_read(outbuf) != 0x00E9) {
      emit("M53-PARSERUTILS: fail codec-decode\n");
      parserutils_charset_codec_destroy(dec);
      return 1;
    }
  }
  parserutils_charset_codec_destroy(dec);
  emit("M53-PARSERUTILS: ok codec-decode\n");

  /* ── A real UTF-8 codec: encode UCS-4 '€' (0x20AC) back to its 3 bytes ── */
  parserutils_charset_codec *enc = NULL;
  if (parserutils_charset_codec_create("UTF-8", &enc) != PARSERUTILS_OK ||
      enc == NULL) {
    emit("M53-PARSERUTILS: fail codec-create\n");
    return 1;
  }
  {
    uint8_t in[4];
    be32_write(in, 0x20AC); /* '€' as big-endian UCS-4 */
    const uint8_t *sp = in;
    size_t slen = sizeof(in);
    uint8_t outbuf[8];
    uint8_t *dp = outbuf;
    size_t dlen = sizeof(outbuf);
    if (parserutils_charset_codec_encode(enc, &sp, &slen, &dp, &dlen) !=
            PARSERUTILS_OK) {
      emit("M53-PARSERUTILS: fail codec-encode\n");
      parserutils_charset_codec_destroy(enc);
      return 1;
    }
    size_t wrote = sizeof(outbuf) - dlen;
    if (wrote != 3 || outbuf[0] != 0xE2 || outbuf[1] != 0x82 ||
        outbuf[2] != 0xAC) {
      emit("M53-PARSERUTILS: fail codec-encode\n");
      parserutils_charset_codec_destroy(enc);
      return 1;
    }
  }
  parserutils_charset_codec_destroy(enc);
  emit("M53-PARSERUTILS: ok codec-encode\n");

  emit("M53-PARSERUTILS: done\n");
  return 0;
}
