#!/bin/sh
# Build libparserutils as a static libparserutils.a for the b1nix userspace ABI.
# Freestanding curated compile of the portable C sources (no autotools), like
# the other ports. libparserutils provides NetSurf's input streams, buffers and
# its OWN bundled charset codecs (8859/ASCII/ext8/UTF-8/UTF-16) — built with
# -DWITHOUT_ICONV_FILTER so it never touches a system iconv (b1nix has none).
# Prints the install dir.
#
# M53 (NetSurf browser platform) — browser lib chain step 2. libhubbub and
# libcss/libdom all parse their input through this.
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libparserutils
NSLIB_VERSION="${PU_VERSION:-0.2.5}"
NSLIB_ARCHIVE=libparserutils.a
# WITHOUT_ICONV_FILTER → use the bundled codecs, no <iconv.h>.
NSLIB_CFLAGS="-DWITHOUT_ICONV_FILTER -Wno-implicit-function-declaration"
NSLIB_HEADERS="tree:include/parserutils"
NSLIB_SOURCES="
  src/charset/aliases.c
  src/charset/codec.c
  src/charset/codecs/codec_8859.c
  src/charset/codecs/codec_ascii.c
  src/charset/codecs/codec_ext8.c
  src/charset/codecs/codec_utf16.c
  src/charset/codecs/codec_utf8.c
  src/charset/encodings/utf16.c
  src/charset/encodings/utf8.c
  src/input/filter.c
  src/input/inputstream.c
  src/utils/buffer.c
  src/utils/errors.c
  src/utils/stack.c
  src/utils/vector.c
"

# Charset-alias table: the upstream build generates src/charset/aliases.inc from
# build/Aliases via a perl script. Run it once (host perl, arch-agnostic).
port_pre_build() {
  PERL_BIN="${PERL:-$(command -v perl)}"
  if [ ! -f "$SRC_DIR/src/charset/aliases.inc" ]; then
    ( cd "$SRC_DIR" && "$PERL_BIN" build/make-aliases.pl ) 1>&2
  fi
}

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
