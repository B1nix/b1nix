#!/bin/sh
# Build FreeType (TrueType + CFF, smooth rasterizer) as a static libfreetype.a
# for the b1nix userspace ABI. Single-object freestanding build per
# docs/INSTALL.ANY; no autotools. Prints the install dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FT_VERSION="${FT_VERSION:-2.13.2}"
TARBALL="freetype-${FT_VERSION}.tar.gz"
URL="https://download.savannah.gnu.org/releases/freetype/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/freetype-${FT_VERSION}"
BUILD_DIR="$ROOT_DIR/build/freetype-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xzf "$tmp" -C "$SRC_PARENT" 1>&2
  # The smooth rasterizer's SSE2 fast path needs <emmintrin.h>, absent in the
  # freestanding sysroot. Force the generic (correct, unaccelerated) path.
  sed -i '' 's/^#  define FT_SSE2 1/#  define FT_SSE2 0/' \
    "$SRC_DIR/src/smooth/ftgrays.c"
fi

# Trimmed module list: TrueType + CFF/OpenType with the smooth rasterizer.
# Drops SDF/SVG/BDF/PCF/PFR/Type1/Type42/winfonts which need extra deps or
# are unused for desktop text. Must match the module umbrellas compiled below.
cat > "$GEN_DIR/ftmodule_b1nix.h" <<'EOF'
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
EOF

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -DFT2_BUILD_LIBRARY
  -DFT_CONFIG_MODULES_H=\"ftmodule_b1nix.h\"
  -I$GEN_DIR -I$SRC_DIR/include -Wno-implicit-function-declaration"

BASE="ftsystem ftinit ftdebug ftbase ftbbox ftglyph ftbitmap ftstroke \
ftsynth ftmm ftgasp ftfstype ftpatent"
MODULES="src/truetype/truetype src/cff/cff src/sfnt/sfnt src/smooth/smooth \
src/raster/raster src/autofit/autofit src/psnames/psnames src/psaux/psaux \
src/pshinter/pshinter src/gzip/ftgzip"

OBJS=""
compile() {
  obj="$OBJ_DIR/$(echo "$1" | tr / _).o"
  clang $CFLAGS -c "$SRC_DIR/$2" -o "$obj"
  OBJS="$OBJS $obj"
}
for b in $BASE; do compile "$b" "src/base/$b.c"; done
for m in $MODULES; do compile "$m" "$m.c"; done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libfreetype.a" $OBJS
cp -r "$SRC_DIR/include/"* "$INSTALL_DIR/" 2>/dev/null
mkdir -p "$INSTALL_DIR/include"
cp -r "$SRC_DIR/include/"* "$INSTALL_DIR/include/"

echo "$INSTALL_DIR"
