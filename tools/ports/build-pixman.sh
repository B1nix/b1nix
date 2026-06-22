#!/bin/sh
# Build pixman (generic C, no SIMD) as a static libpixman-1.a for the b1nix
# userspace ABI. Freestanding compile of the portable subset, like the other
# graphics ports — meson/autotools are bypassed. Prints the install dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
PIXMAN_VERSION="${PIXMAN_VERSION:-0.42.2}"
TARBALL="pixman-${PIXMAN_VERSION}.tar.gz"
URL="https://gitlab.freedesktop.org/pixman/pixman/-/archive/pixman-${PIXMAN_VERSION}/pixman-pixman-${PIXMAN_VERSION}.tar.gz"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/pixman-src"
SRC_DIR="$SRC_PARENT/pixman-pixman-${PIXMAN_VERSION}"
P="$SRC_DIR/pixman"
BUILD_DIR="$ROOT_DIR/build/pixman-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"
LIBM_DIR="$($ROOT_DIR/tools/ports/build-openlibm.sh)"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/include/pixman-1" \
  "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
fi

MAJOR=$(echo "$PIXMAN_VERSION" | cut -d. -f1)
MINOR=$(echo "$PIXMAN_VERSION" | cut -d. -f2)
MICRO=$(echo "$PIXMAN_VERSION" | cut -d. -f3)
sed -e "s/@PIXMAN_VERSION_MAJOR@/$MAJOR/g" \
    -e "s/@PIXMAN_VERSION_MINOR@/$MINOR/g" \
    -e "s/@PIXMAN_VERSION_MICRO@/$MICRO/g" \
    "$P/pixman-version.h.in" > "$GEN_DIR/pixman-version.h"

# Minimal config.h. `PACKAGE` is the sentinel pixman-private.h checks for.
# No SIMD, no threads (single-threaded smoke) -> pixman uses its generic paths.
cat > "$GEN_DIR/config.h" <<EOF
#define PACKAGE "pixman"
#define PACKAGE_VERSION "$PIXMAN_VERSION"
#define PACKAGE_BUGREPORT ""
EOF

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -I$LIBM_DIR/include -O2 -fno-strict-aliasing -Db1nix -DHAVE_CONFIG_H
  -I$GEN_DIR -I$P -Wno-implicit-function-declaration -DPIXMAN_NO_TLS"

# Generic subset: everything except the SIMD implementations. The per-arch
# dispatch stubs (pixman-x86/arm/ppc/mips.c) stay -- they no-op without the
# USE_* SIMD macros and are called unconditionally by _pixman_choose_implementation.
OBJS=""
for src in "$P"/*.c; do
  base=$(basename "$src" .c)
  case "$base" in
    pixman-mmx|pixman-sse2|pixman-ssse3|pixman-arm-neon|pixman-arm-simd|\
pixman-vmx|pixman-mips-dspr2) continue;;
    # Template, #included by pixman-region16.c / pixman-region32.c.
    pixman-region) continue;;
  esac
  obj="$OBJ_DIR/$base.o"
  clang $CFLAGS -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libpixman-1.a" $OBJS
cp "$P/pixman.h" "$GEN_DIR/pixman-version.h" "$INSTALL_DIR/include/pixman-1/"

echo "$INSTALL_DIR"
