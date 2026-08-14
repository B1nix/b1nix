#!/bin/sh
# Stage the M91 Skia shared-library dependency graph into
# userspace/build/$ARCH/ so the kernel initramfs can embed them.
#
# Produces (all dynamically linked, SysV DT_HASH so the in-kernel eager linker
# can resolve them — see tools/ports/b1nix-cross-cc.sh --hash-style=both):
#   libskia.so       — Skia itself (stripped), from build-skia.sh (ninja)
#   libraw_ptr.so    — Skia's raw_ptr component, from ninja
#   libfontconfig.so — b1nix fontconfig + freetype + zlib + expat, relinked here
#                      (libskia.so DT_NEEDEDs it for its Fc* symbols)
#   libGLESv2.so     — thin GL stub (real GL entrypoints are folded into the
#   libEGL.so          Skia demo exe over Mesa OSMesa; see build-m91-skia-demo.sh)
#
# x86_64 only. build-skia.sh (.skia-built) must have run first.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT_DIR/tools/toolchain/env.sh"

if [ "$B1NIX_ARCH" != "x86_64" ]; then
  echo "build-skia-shared-deps: x86_64 only (ARCH=$B1NIX_ARCH)" >&2
  exit 0
fi

TRIPLET="$B1NIX_TRIPLET"
SKIA_OUT="$(ls -d "$ROOT_DIR/build/$B1NIX_ARCH/ports/skia/build/out/b1nix" "$ROOT_DIR/build/src/skia/out/b1nix" 2>/dev/null | head -1)"
UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
CROSSL="$ROOT_DIR/build/$B1NIX_ARCH/ports/musl/install/lib"
[ -d "$CROSSL" ] || CROSSL="$TOOLCHAIN_BUILD_HOME/cross/lib"
LLD="${B1NIX_LLD:-$(command -v ld.lld 2>/dev/null || echo /usr/bin/ld.lld)}"
STRIP="$(command -v llvm-strip 2>/dev/null || command -v strip 2>/dev/null || echo strip)"

# With static linking, libskia.a is built instead of libskia.so.
# Create minimal placeholder .so files for any code that still expects them.
mkdir -p "$UB"

if [ -f "$SKIA_OUT/libskia.so" ]; then
  # Component build — copy shared libs
  cp -f "$SKIA_OUT/libskia.so" "$UB/libskia.so"
  "$STRIP" --strip-unneeded "$UB/libskia.so" 2>/dev/null || true
  [ -f "$SKIA_OUT/libraw_ptr.so" ] && cp -f "$SKIA_OUT/libraw_ptr.so" "$UB/libraw_ptr.so"
else
  # Static build — create empty placeholders
  echo "build-skia-shared-deps: no libskia.so (static build), creating placeholders" >&2
  echo "void __skia_placeholder(void) {}" | \
    clang -shared -x c - -o "$UB/libskia.so" 2>/dev/null || true
  echo "void __raw_ptr_placeholder(void) {}" | \
    clang -shared -x c - -o "$UB/libraw_ptr.so" 2>/dev/null || true
fi

# --- 2) libfontconfig.so (Alpine's, copied) ----------------------------------
# Skia links -lfontconfig for its Fc* symbols. This used to fold the fontconfig
# port and its freetype/zlib/expat dependencies into one shared object by hand;
# fontconfig is an Alpine package now and already ships that shared library, so
# it is copied rather than rebuilt. Folding its archive is no longer even
# possible: Alpine's static libfontconfig refers to its own data and to libc's
# stderr with PC32 relocations, which a shared object cannot resolve.
#
# The copy keeps the name Skia's -lfontconfig expects; its SONAME is
# libfontconfig.so.1, which is what consumers record and what the root-image
# rule stages, along with the libfreetype.so.6 and libexpat.so.1 it needs.
FC_SO="$(ls "$ROOT_DIR/build/$B1NIX_ARCH/pkg/fontconfig/lib/libfontconfig.so."[0-9]* 2>/dev/null | head -1)"
[ -n "$FC_SO" ] || {
  echo "build-skia-shared-deps: no libfontconfig.so.* — run tools/packages/pkg-prefix.sh fontconfig" >&2
  exit 1
}
cp -f "$FC_SO" "$UB/libfontconfig.so"

# --- 4) libGLESv2.so — real Mesa GL entry points (for Dawn dlopen) -----------
# Dawn's OpenGL ES backend loads libGLESv2.so via dlopen at runtime.
# We fold Mesa's libglapi_static.a (1971 GL entry points) into a shared lib.
MESA_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/mesa/install/lib"
GLAPI_A="$MESA_DIR/libglapi_static.a"
[ -f "$GLAPI_A" ] || { echo "build-skia-shared-deps: missing $GLAPI_A (run build-mesa.sh)" >&2; exit 1; }
"$LLD" -shared -m elf_x86_64 --hash-style=both -soname libGLESv2.so \
  --whole-archive "$GLAPI_A" --no-whole-archive \
  -L "$CROSSL" -lc -lm \
  --allow-shlib-undefined \
  -o "$UB/libGLESv2.so"

