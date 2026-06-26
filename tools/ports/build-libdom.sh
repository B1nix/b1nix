#!/bin/sh
# Build libdom as a static libdom.a for the b1nix userspace ABI. Freestanding
# curated compile of the portable C sources (no autotools), like the other
# ports. libdom is NetSurf's DOM; the smoke builds a document from HTML via the
# bundled libhubbub binding. We compile the core DOM (src/**) plus the hubbub
# binding (bindings/hubbub/parser.c); the libxml2/expat bindings are not built
# here (HTML rendering only needs the hubbub path). Prints the install dir.
#
# M53 (NetSurf browser platform) — browser lib chain step 5. Depends on
# build-libhubbub.sh, build-libwapcaplet.sh, build-libparserutils.sh.
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libdom
NSLIB_VERSION="${DOM_VERSION:-0.4.2}"
NSLIB_ARCHIVE=libdom.a
# Dep include order matches the original CFLAGS: hubbub, parserutils, wapcaplet.
NSLIB_DEPS="build-libhubbub.sh build-libparserutils.sh build-libwapcaplet.sh"
# -Ibindings/hubbub for the binding's local headers; deps add <hubbub/...>,
# <parserutils/...>, <libwapcaplet/...>.
NSLIB_INC_DIRS="include src bindings/hubbub"
NSLIB_FIND_DIR=src
NSLIB_EXTRA_SOURCES="bindings/hubbub/parser.c"
NSLIB_CFLAGS="-Wno-implicit-function-declaration"

port_install_headers() {
  cp -R "$SRC_DIR/include/dom" "$INSTALL_DIR/include/"
  mkdir -p "$INSTALL_DIR/include/dom/bindings/hubbub"
  cp "$SRC_DIR/bindings/hubbub/errors.h" "$SRC_DIR/bindings/hubbub/parser.h" \
     "$INSTALL_DIR/include/dom/bindings/hubbub/"
}

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
