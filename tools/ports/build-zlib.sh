#!/bin/sh
# Build zlib as a static libz.a for the b1nix userspace ABI. Freestanding compile
# of the portable C sources (no autotools/configure), like the other ports. The
# core in-memory codec plus the gz* file wrappers (NetSurf's utils/hashtable.c
# reads gzipped data via gzopen/gzgets/gzclose). Prints the install dir.
#
# First M53 (NetSurf browser platform) image-codec dependency.
# Build logic lives in tools/ports/drivers/cport.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=zlib
CPORT_ARCHIVE=libz.a
CPORT_SRC_PARENT="$ROOT_DIR/build/zlib-src"
VER="${ZLIB_VERSION:-1.3.1}"
CPORT_SRCNAME="zlib-${VER}"
CPORT_TARBALL="zlib-${VER}.tar.gz"
CPORT_URL="https://github.com/madler/zlib/archive/refs/tags/v${VER}.tar.gz"
# zlib's zconf.h ships ready to use. HAVE_HIDDEN keeps internal symbols off the
# export surface.
CPORT_SOURCES="adler32.c compress.c crc32.c deflate.c infback.c inffast.c \
inflate.c inftrees.c trees.c uncompr.c zutil.c gzlib.c gzread.c gzwrite.c gzclose.c"
CPORT_HEADERS="flat:zlib.h flat:zconf.h"

port_pre_build() {
  CPORT_CFLAGS="-DHAVE_HIDDEN -I$SRC_DIR -Wno-implicit-function-declaration"
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
