#!/bin/sh
# Build libnsbmp as a static libnsbmp.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). libnsbmp is NetSurf's standalone
# BMP/ICO decoder; the browser registers it as an image content handler. Prints
# the install dir.
#
# M53 (NetSurf browser platform) — browser image decoder (step 6).
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libnsbmp
NSLIB_VERSION="${NSBMP_VERSION:-0.1.7}"
NSLIB_ARCHIVE=libnsbmp.a
NSLIB_SOURCES="src/libnsbmp.c"
NSLIB_CFLAGS="-Wno-implicit-function-declaration"
NSLIB_HEADERS="flat:include/libnsbmp.h"

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
