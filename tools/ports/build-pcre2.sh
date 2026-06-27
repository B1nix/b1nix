#!/bin/sh
# Build upstream PCRE2 (8-bit static library) for the b1nix userspace ABI.
# Uses the autotools driver (tools/ports/drivers/autotools.sh).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

AUTOTOOLS_NAME=pcre2
AUTOTOOLS_VERSION="${PCRE2_VERSION:-10.44}"
AUTOTOOLS_URL="https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${AUTOTOOLS_VERSION}/pcre2-${AUTOTOOLS_VERSION}.tar.gz"
AUTOTOOLS_CONFIGURE="--disable-shared --enable-static --disable-jit --disable-pcre2-16 --disable-pcre2-32 --disable-pcre2grep-libz --disable-pcre2grep-libbz2 --disable-pcre2test-libedit --disable-pcre2test-libreadline"
AUTOTOOLS_MAKE="libpcre2-8.la"
AUTOTOOLS_INSTALL="install-libLTLIBRARIES install-nodist_includeHEADERS"

port_install() {
  make -C "$BUILD_DIR" install-libLTLIBRARIES install-nodist_includeHEADERS 1>&2 \
    || make -C "$BUILD_DIR" install 1>&2
}

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"
