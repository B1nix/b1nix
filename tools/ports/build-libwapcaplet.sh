#!/bin/sh
# Build libwapcaplet as a static liblwc.a for the b1nix userspace ABI.
# Freestanding compile of the single portable C source (no autotools), like the
# other ports. libwapcaplet is NetSurf's string-internment / hash-table library
# and the smallest, lowest dependency in the browser lib chain. Prints the
# install dir.
#
# M53 (NetSurf browser platform) — first NetSurf-own library dependency.
# Build logic lives in tools/ports/drivers/nslib.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

NSLIB_NAME=libwapcaplet
NSLIB_VERSION="${LWC_VERSION:-0.4.3}"
NSLIB_ARCHIVE=liblwc.a
NSLIB_SOURCES="src/libwapcaplet.c"
NSLIB_CFLAGS="-Wno-implicit-function-declaration"
NSLIB_HEADERS="sub:libwapcaplet:include/libwapcaplet/libwapcaplet.h"

. "$ROOT_DIR/tools/ports/drivers/nslib.sh"
