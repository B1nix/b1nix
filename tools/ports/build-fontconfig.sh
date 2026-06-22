#!/bin/sh
# Build Fontconfig as a static libfontconfig.a for the b1nix userspace ABI.
# Reproduces the meson code-generation (fcobjshash via cpp+gperf, alias headers
# and fccase.h via the bundled python scripts), then freestanding-compiles the
# sources. Needs expat + FreeType. Prints the install dir.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
FC_VERSION="${FC_VERSION:-2.14.2}"
TARBALL="fontconfig-${FC_VERSION}.tar.xz"
URL="https://www.freedesktop.org/software/fontconfig/release/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
GPERF="${GPERF:-$(command -v /opt/homebrew/opt/gperf/bin/gperf 2>/dev/null || command -v gperf)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/fontconfig-${FC_VERSION}"
BUILD_DIR="$ROOT_DIR/build/fontconfig-b1nix/$B1NIX_TRIPLET"
OBJ_DIR="$BUILD_DIR/obj"
GEN_DIR="$BUILD_DIR/gen"
INSTALL_DIR="$BUILD_DIR/install"
EXPAT_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-expat.sh")"
FT_DIR="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/build-freetype.sh")"

mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$GEN_DIR" "$INSTALL_DIR/include/fontconfig" \
  "$INSTALL_DIR/lib"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xJf "$tmp" -C "$SRC_PARENT" 1>&2
fi

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

# fcobjshash: preprocess the gperf template, then run gperf. cutout.py wants a
# meson buildroot to read host c_args from; give it an empty options file so it
# uses none, and pass the cpp command as separate argv words.
mkdir -p "$GEN_DIR/meson-info"
echo '[]' > "$GEN_DIR/meson-info/intro-buildoptions.json"
python3 "$SRC_DIR/src/cutout.py" "$SRC_DIR/src/fcobjshash.gperf.h" \
  "$GEN_DIR/fcobjshash.gperf" "$GEN_DIR" \
  cc -E -P -I"$SRC_DIR" -I"$SRC_DIR/src" -I"$GEN_DIR" -I"$INSTALL_DIR/include"
"$GPERF" --pic -m 100 "$GEN_DIR/fcobjshash.gperf" \
  --output-file "$GEN_DIR/fcobjshash.h"

# Public headers fontconfig sources include as <fontconfig/...>.
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
#define FONTCONFIG_VERSION "$FC_VERSION"
#ifndef timercmp
#define timercmp(a, b, CMP)                                                   \
  (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec CMP(b)->tv_usec)              \
                                : ((a)->tv_sec CMP(b)->tv_sec))
#endif
EOF

if [ "$B1NIX_ARCH" = "x86" ]; then
  TARGET="i686-unknown-elf"
else
  TARGET="x86_64-unknown-elf"
fi

CFLAGS="--target=$TARGET -ffreestanding -fno-builtin -fno-stack-protector
  -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
  -O2 -fno-strict-aliasing -Db1nix -DHAVE_CONFIG_H
  -I$GEN_DIR -I$SRC_DIR -I$SRC_DIR/src -I$INSTALL_DIR/include
  -I$EXPAT_DIR/include -I$FT_DIR/include
  -DHAVE_EXPAT=1 -DFcPublic= -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-implicit-int -Wno-incompatible-function-pointer-types"

OBJS=""
for src in "$SRC_DIR/src"/fc*.c "$SRC_DIR/src/ftglue.c"; do
  base=$(basename "$src" .c)
  case "$base" in *test*) continue;; esac
  obj="$OBJ_DIR/$base.o"
  clang $CFLAGS -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086
"$AR_BIN" rcs "$INSTALL_DIR/lib/libfontconfig.a" $OBJS
echo "$INSTALL_DIR"
