#!/bin/sh
# Build FreeType (TrueType + CFF, smooth rasterizer) as a static libfreetype.a
# for the b1nix userspace ABI. Single-object freestanding build per
# docs/INSTALL.ANY; no autotools. Prints the install dir.
#
# Build logic lives in tools/ports/drivers/cport.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=freetype
CPORT_ARCHIVE=libfreetype.a
VER="${FT_VERSION:-2.13.2}"
CPORT_SRCNAME="freetype-${VER}"
CPORT_TARBALL="freetype-${VER}.tar.gz"
CPORT_URL="https://download-mirror.savannah.gnu.org/releases/freetype/freetype-${VER}.tar.gz"
# The smooth rasterizer's SSE2 fast path needs <emmintrin.h>; force generic.
PATCHES="freetype/no-sse2.sh"

# Trimmed module list: TrueType + CFF/OpenType with the smooth rasterizer.
# Drops SDF/SVG/BDF/PCF/PFR/Type1/Type42/winfonts (extra deps / unused for
# desktop text). Must match the module umbrellas compiled below.
port_pre_build() {
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
  CPORT_CFLAGS="-DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H=\"ftmodule_b1nix.h\" \
-I$GEN_DIR -I$SRC_DIR/include -Wno-implicit-function-declaration"
}

port_build() {
  BASE="ftsystem ftinit ftdebug ftbase ftbbox ftglyph ftbitmap ftstroke \
ftsynth ftmm ftgasp ftfstype ftpatent"
  MODULES="src/truetype/truetype src/cff/cff src/sfnt/sfnt src/smooth/smooth \
src/raster/raster src/autofit/autofit src/psnames/psnames src/psaux/psaux \
src/pshinter/pshinter src/gzip/ftgzip"
  for b in $BASE; do cport_cc "$SRC_DIR/src/base/$b.c"; done
  for m in $MODULES; do cport_cc "$SRC_DIR/$m.c"; done
}

port_install_headers() {
  cp -r "$SRC_DIR/include/"* "$INSTALL_DIR/" 2>/dev/null
  mkdir -p "$INSTALL_DIR/include"
  cp -r "$SRC_DIR/include/"* "$INSTALL_DIR/include/"
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
