#!/bin/sh
# Build static libunistring for the b1nix userspace ABI.
# Uses the autotools driver (tools/ports/drivers/autotools.sh).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

AUTOTOOLS_NAME=libunistring
AUTOTOOLS_VERSION="${LIBUNISTRING_VERSION:-1.2}"
AUTOTOOLS_URL="https://ftp.gnu.org/gnu/libunistring/libunistring-${AUTOTOOLS_VERSION}.tar.gz"
AUTOTOOLS_CONFIGURE="--disable-shared --enable-static --disable-nls"
AUTOTOOLS_MAKE="lib"

PATCHES="libunistring/b1nix-config-sub.sh libunistring/b1nix-fseterr.sh"

port_install() {
  make -C "$BUILD_DIR/lib" install 1>&2
}

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"
