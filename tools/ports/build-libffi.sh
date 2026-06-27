#!/bin/sh
# Build upstream libffi as a static library for the b1nix userspace ABI.
# Uses the autotools driver (tools/ports/drivers/autotools.sh).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

AUTOTOOLS_NAME=libffi
AUTOTOOLS_VERSION="${LIBFFI_VERSION:-3.5.2}"
AUTOTOOLS_URL="https://github.com/libffi/libffi/releases/download/v${AUTOTOOLS_VERSION}/libffi-${AUTOTOOLS_VERSION}.tar.gz"
AUTOTOOLS_CONFIGURE="--disable-shared --enable-static --disable-docs --disable-multi-os-directory"

port_build() {
  # libffi has a parallel-build race, build serially
  make -C "$BUILD_DIR" -j1 1>&2
}

. "$ROOT_DIR/tools/ports/drivers/autotools.sh"
