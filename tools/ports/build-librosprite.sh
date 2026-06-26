#!/bin/sh
# Build librosprite as a static librosprite.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). librosprite decodes RISC-OS
# sprite images; NetSurf registers it as the image/x-riscos-sprite content
# handler. Pure C, stdlib only. Prints the install dir.
#
# M53 (NetSurf browser platform) — RISC-OS sprite image decoder.
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=librosprite
NSLIB_ARCHIVE=librosprite.a
NSLIB_SOURCE=bundle
NSLIB_BUNDLE_SUBDIR=librosprite
NSLIB_SENTINEL=src/librosprite.c
NSLIB_SOURCES="src/librosprite.c"
NSLIB_CFLAGS="-Wno-implicit-function-declaration"
NSLIB_HEADERS="flat:include/librosprite.h"

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
