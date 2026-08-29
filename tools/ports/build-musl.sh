#!/bin/sh
# tools/ports/build-musl.sh - Install musl libc, dev headers, and Linux UAPI headers via Alpine packages.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"

"$ROOT_DIR/tools/packages/pkg-prefix.sh" musl >/dev/null

mkdir -p "$ROOT_DIR/build/$ARCH/ports/musl"
# A real directory left here by an older layout is not replaced by `ln -sfn`:
# the link is created inside it instead, and every consumer keeps reading the
# stale copy. That is what left the aarch64 sysroot without the Linux UAPI
# headers -- <asm/unistd.h> and <linux/fb.h> were simply absent, so BusyBox's
# ionice and m47_smoke would not compile while x86_64 built both fine.
[ -L "$ROOT_DIR/build/$ARCH/ports/musl/install" ] ||
    rm -rf "$ROOT_DIR/build/$ARCH/ports/musl/install"
ln -sfn "../../pkg/musl" "$ROOT_DIR/build/$ARCH/ports/musl/install"

echo "$ROOT_DIR/build/$ARCH/pkg/musl"
