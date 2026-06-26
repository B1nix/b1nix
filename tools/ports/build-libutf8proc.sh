#!/bin/sh
# Build utf8proc as a static libutf8proc.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). utf8proc is a single-file Unicode
# library; NetSurf uses it (utils/idna.c) for IDNA Unicode normalisation /
# property lookup. It ships inside the netsurf-all bundle. The header installs
# under libutf8proc/ (NetSurf includes <libutf8proc/utf8proc.h>). Prints the
# install dir.
#
# M53 (NetSurf browser platform) — Unicode / IDNA support.
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libutf8proc
NSLIB_ARCHIVE=libutf8proc.a
NSLIB_SOURCE=bundle
NSLIB_BUNDLE_SUBDIR=libutf8proc
NSLIB_SENTINEL=src/utf8proc.c
NSLIB_SOURCES="src/utf8proc.c"
# UTF8PROC_STATIC avoids dllexport attributes; utf8proc.c #includes utf8proc_data.c.
NSLIB_INC_DIRS="src include/libutf8proc"
NSLIB_CFLAGS="-DUTF8PROC_STATIC -Wno-implicit-function-declaration"
NSLIB_HEADERS="sub:libutf8proc:include/libutf8proc/utf8proc.h"

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
