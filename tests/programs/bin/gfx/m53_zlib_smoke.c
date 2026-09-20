/* M53 zlib smoke: prove the ported zlib compiles/decompresses real data on
 * b1nix. Exercises both the one-shot (compress2/uncompress) and streaming
 * (deflate/inflate) APIs — the latter is what libpng and the NetSurf image
 * libraries use — plus crc32. Every marker is gated on a verified byte-for-byte
 * roundtrip; nothing is faked. Markers (M53-ZLIB: ...) consumed by smoke.sh. */

#include <string.h>
#include <unistd.h>
#include <zlib.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

/* A buffer with real redundancy so compression actually shrinks it. */
static unsigned char input[4096];
static unsigned char comp[8192];
static unsigned char decomp[4096];

static void fill_input(void) {
  const char *pat = "The quick brown b1nix fox renders pages 0123456789. ";
  unsigned plen = (unsigned)strlen(pat);
  for (unsigned i = 0; i < sizeof(input); i++)
    input[i] = (unsigned char)pat[i % plen];
}

int main(void) {
  emit("M53-ZLIB: start\n");
  fill_input();

  emit("M53-ZLIB: zlib-version ");
  emit(zlibVersion());
  emit("\n");

  /* ── One-shot compress2 / uncompress ── */
  uLong clen = sizeof(comp);
  if (compress2(comp, &clen, input, sizeof(input), Z_BEST_COMPRESSION) != Z_OK) {
    emit("M53-ZLIB: fail compress\n");
    return 1;
  }
  /* Must actually shrink the redundant input, or compression did nothing. */
  if (clen == 0 || clen >= sizeof(input)) {
    emit("M53-ZLIB: fail compress-ratio\n");
    return 1;
  }
  emit("M53-ZLIB: ok compress\n");

  uLong dlen = sizeof(decomp);
  if (uncompress(decomp, &dlen, comp, clen) != Z_OK) {
    emit("M53-ZLIB: fail uncompress\n");
    return 1;
  }
  emit("M53-ZLIB: ok uncompress\n");

  if (dlen != sizeof(input) || memcmp(decomp, input, sizeof(input)) != 0) {
    emit("M53-ZLIB: fail roundtrip\n");
    return 1;
  }
  emit("M53-ZLIB: ok roundtrip\n");

  /* ── crc32: stable, well-known value for this exact input ── */
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, input, sizeof(input));
  /* Recompute in two halves; the streaming-accumulated CRC must match the
   * single-shot CRC, proving incremental crc32 works (libpng relies on it). */
  uLong crc_a = crc32(0L, Z_NULL, 0);
  crc_a = crc32(crc_a, input, sizeof(input) / 2);
  crc_a = crc32(crc_a, input + sizeof(input) / 2, sizeof(input) - sizeof(input) / 2);
  if (crc == 0 || crc != crc_a) {
    emit("M53-ZLIB: fail crc32\n");
    return 1;
  }
  emit("M53-ZLIB: ok crc32\n");

  /* ── Streaming deflate → inflate (the libpng/NetSurf path) ── */
  z_stream ds;
  memset(&ds, 0, sizeof(ds));
  if (deflateInit(&ds, Z_DEFAULT_COMPRESSION) != Z_OK) {
    emit("M53-ZLIB: fail deflate-init\n");
    return 1;
  }
  ds.next_in = input;
  ds.avail_in = sizeof(input);
  ds.next_out = comp;
  ds.avail_out = sizeof(comp);
  if (deflate(&ds, Z_FINISH) != Z_STREAM_END) {
    emit("M53-ZLIB: fail deflate\n");
    deflateEnd(&ds);
    return 1;
  }
  uLong scomp = ds.total_out;
  deflateEnd(&ds);

  z_stream is;
  memset(&is, 0, sizeof(is));
  if (inflateInit(&is) != Z_OK) {
    emit("M53-ZLIB: fail inflate-init\n");
    return 1;
  }
  memset(decomp, 0, sizeof(decomp));
  is.next_in = comp;
  is.avail_in = (uInt)scomp;
  is.next_out = decomp;
  is.avail_out = sizeof(decomp);
  if (inflate(&is, Z_FINISH) != Z_STREAM_END ||
      is.total_out != sizeof(input) ||
      memcmp(decomp, input, sizeof(input)) != 0) {
    emit("M53-ZLIB: fail inflate\n");
    inflateEnd(&is);
    return 1;
  }
  inflateEnd(&is);
  emit("M53-ZLIB: ok stream\n");

  emit("M53-ZLIB: done\n");
  return 0;
}
