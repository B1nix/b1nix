#!/bin/sh
# Build libnsgif as a static libnsgif.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). libnsgif is NetSurf's standalone
# GIF (+LZW) decoder; the browser registers it as an image content handler.
# Prints the install dir.
#
# M53 (NetSurf browser platform) — browser image decoder (step 6).
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libnsgif
NSLIB_VERSION="${NSGIF_VERSION:-1.0.0}"
NSLIB_ARCHIVE=libnsgif.a
NSLIB_SOURCES="src/gif.c src/lzw.c"
NSLIB_CFLAGS="-Wno-implicit-function-declaration"
NSLIB_HEADERS="flat:include/nsgif.h"

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
