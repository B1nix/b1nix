#!/bin/sh
# Build Fontconfig as a static libfontconfig.a for the b1nix userspace ABI.
# Uses the cport driver (tools/ports/drivers/cport.sh).
# Reproduces the meson code-generation (fcobjshash via cpp+gperf, alias headers
# and fccase.h via the bundled python scripts), then freestanding-compiles the
# sources. Needs expat + FreeType.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=fontconfig
CPORT_ARCHIVE=libfontconfig.a
CPORT_SRCNAME="fontconfig-${FC_VERSION:-2.14.2}"
CPORT_URL="https://www.freedesktop.org/software/fontconfig/release/fontconfig-${FC_VERSION:-2.14.2}.tar.xz"
CPORT_TARBALL="fontconfig-${FC_VERSION:-2.14.2}.tar.xz"
CPORT_HEADERS="tree:fontconfig"

GPERF="${GPERF:-$(command -v /opt/homebrew/opt/gperf/bin/gperf 2>/dev/null || command -v gperf)}"

port_pre_build() {
  EXPAT_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-expat.sh")"
  FT_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-freetype.sh")"
  mkdir -p "$INSTALL_DIR/include/fontconfig"

  # --- code generation (host python3 + cpp + gperf) ---
  cp "$SRC_DIR/fc-lang/fclang.h" "$GEN_DIR/"
  python3 "$SRC_DIR/fc-case/fc-case.py" "$SRC_DIR/fc-case/CaseFolding.txt" \
    --template "$SRC_DIR/fc-case/fccase.tmpl.h" --output "$GEN_DIR/fccase.h"
  python3 "$SRC_DIR/src/makealias.py" "$SRC_DIR/src" \
    "$GEN_DIR/fcalias.h" "$GEN_DIR/fcaliastail.h" \
    "$SRC_DIR/fontconfig/fontconfig.h" "$SRC_DIR/src/fcdeprecate.h" \
    "$SRC_DIR/fontconfig/fcprivate.h"
  python3 "$SRC_DIR/src/makealias.py" "$SRC_DIR/src" \
    "$GEN_DIR/fcftalias.h" "$GEN_DIR/fcftaliastail.h" \
    "$SRC_DIR/fontconfig/fcfreetype.h"

  mkdir -p "$GEN_DIR/meson-info"
  echo '[]' > "$GEN_DIR/meson-info/intro-buildoptions.json"
  python3 "$SRC_DIR/src/cutout.py" "$SRC_DIR/src/fcobjshash.gperf.h" \
    "$GEN_DIR/fcobjshash.gperf" "$GEN_DIR" \
    cc -E -P -I"$SRC_DIR" -I"$SRC_DIR/src" -I"$GEN_DIR" -I"$INSTALL_DIR/include"
  "$GPERF" --pic -m 100 "$GEN_DIR/fcobjshash.gperf" \
    --output-file "$GEN_DIR/fcobjshash.h"

  cp "$SRC_DIR/fontconfig"/*.h "$INSTALL_DIR/include/fontconfig/"

  if [ "$B1NIX_ARCH" = "x86" ]; then PTR=4; ALN=4; else PTR=8; ALN=8; fi
  cat > "$GEN_DIR/config.h" <<EOF
#define HAVE_CONFIG_H 1
#define SIZEOF_VOID_P $PTR
#define ALIGNOF_VOID_P $PTR
#define ALIGNOF_DOUBLE $ALN
#define FONTCONFIG_PATH "/etc/fonts"
#define CONFIGDIR "/etc/fonts/conf.d"
#define FC_CACHEDIR "/var/cache/fontconfig"
#define FC_TEMPLATEDIR "/etc/fonts/conf.avail"
#define FC_DEFAULT_FONTS "<dir>/share/fonts</dir>"
#define FC_FONTPATH ""
#define SYSCONFDIR "/etc"
#define FC_GPERF_SIZE_T size_t
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_DIRENT_H 1
#define HAVE_FCNTL_H 1
#define HAVE_RAND 1
#define HAVE_MKSTEMP 1
#define HAVE_STRUCT_STAT_ST_MTIM 1
#define FLEXIBLE_ARRAY_MEMBER
#define FC_CACHE_VERSION_NUMBER 9
#define FC_CACHE_VERSION "9"
#define FONTCONFIG_VERSION "${FC_VERSION:-2.14.2}"
#ifndef timercmp
#define timercmp(a, b, CMP) \
  (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec CMP(b)->tv_usec) \
                                : ((a)->tv_sec CMP(b)->tv_sec))
#endif
EOF

  CPORT_CFLAGS="-DHAVE_CONFIG_H -I$GEN_DIR -I$SRC_DIR -I$SRC_DIR/src -I$INSTALL_DIR/include \
    -I$EXPAT_DIR/include -I$FT_DIR/include \
    -DHAVE_EXPAT=1 -DFcPublic= -Wno-implicit-function-declaration -Wno-incompatible-pointer-types \
    -Wno-int-conversion -Wno-implicit-int -Wno-incompatible-function-pointer-types"
}

port_build() {
  for src in "$SRC_DIR/src"/fc*.c "$SRC_DIR/src/ftglue.c"; do
    base=$(basename "$src" .c)
    case "$base" in *test*) continue;; esac
    cport_cc "$src"
  done
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
