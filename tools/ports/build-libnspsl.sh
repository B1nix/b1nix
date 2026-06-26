#!/bin/sh
# Build libnspsl as a static libnspsl.a for the b1nix userspace ABI.
# Freestanding curated compile (no autotools). libnspsl is NetSurf's public
# suffix list lookup; the browser uses it to scope cookies/same-origin to a
# registrable domain. The perfect-hash table (psl.inc) ships pre-generated in
# the source, so no perl/gperf step is needed. Prints the install dir.
#
# M53 (NetSurf browser platform) — public suffix list.
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libnspsl
NSLIB_ARCHIVE=libnspsl.a
NSLIB_SOURCE=bundle
NSLIB_BUNDLE_SUBDIR=libnspsl
NSLIB_SENTINEL=src/nspsl.c
NSLIB_SOURCES="src/nspsl.c"
NSLIB_HEADERS="flat:include/nspsl.h"

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
