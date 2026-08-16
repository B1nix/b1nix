#!/bin/sh
# tools/ports/build-musl.sh - Install musl libc, dev headers, and Linux UAPI headers via Alpine packages.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"

"$ROOT_DIR/tools/packages/pkg-prefix.sh" musl >/dev/null

mkdir -p "$ROOT_DIR/build/$ARCH/ports/musl"
ln -sfn "../../pkg/musl" "$ROOT_DIR/build/$ARCH/ports/musl/install"

echo "$ROOT_DIR/build/$ARCH/pkg/musl"
