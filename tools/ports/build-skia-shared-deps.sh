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
#   libb1gui.so      — b1nix GUI client library, relinked from libb1gui.a
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
SKIA_OUT="$ROOT_DIR/build/ports-src/skia/out/b1nix"
UB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"
CROSSL="$ROOT_DIR/build/toolchain_build/$TRIPLET/cross/$TRIPLET/lib"
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

# --- 2) libfontconfig.so (b1nix ports; SysV+GNU hash) ------------------------
# Skia links -lfontconfig for its Fc* symbols. We fold the b1nix fontconfig
# port plus its freetype/zlib/expat dependencies into one .so exporting Fc*.
# The b1nix freetype has no libpng/Chromium-zlib deps, so the graph stays clean.
FC_A="$ROOT_DIR/build/fontconfig-b1nix/$TRIPLET/install/lib/libfontconfig.a"
FT_A="$ROOT_DIR/build/freetype-b1nix/$TRIPLET/install/lib/libfreetype.a"
Z_A="$ROOT_DIR/build/zlib-b1nix/$TRIPLET/install/lib/libz.a"
EX_A="$ROOT_DIR/build/expat-b1nix/$TRIPLET/install/lib/libexpat.a"
for a in "$FC_A" "$FT_A" "$Z_A" "$EX_A"; do
  [ -f "$a" ] || { echo "build-skia-shared-deps: missing archive $a" >&2; exit 1; }
done
"$LLD" -shared -m elf_x86_64 --hash-style=both -soname libfontconfig.so \
  --whole-archive "$FC_A" --no-whole-archive \
  "$FT_A" "$Z_A" "$EX_A" \
  -L "$CROSSL" -lc \
  --allow-shlib-undefined \
  -o "$UB/libfontconfig.so"

# --- 3) libb1gui.so (from libb1gui.a; SysV+GNU hash) -------------------------
B1GUI_A="$UB/libb1gui.a"
[ -f "$B1GUI_A" ] || { echo "build-skia-shared-deps: missing $B1GUI_A (run make -C userspace)" >&2; exit 1; }
"$LLD" -shared -m elf_x86_64 --hash-style=both -soname libb1gui.so \
  --whole-archive "$B1GUI_A" --no-whole-archive \
  -L "$CROSSL" -lc \
  --allow-shlib-undefined \
  -o "$UB/libb1gui.so"

# --- 4) libGLESv2.so — real Mesa GL entry points (for Dawn dlopen) -----------
# Dawn's OpenGL ES backend loads libGLESv2.so via dlopen at runtime.
# We fold Mesa's libglapi_static.a (1971 GL entry points) into a shared lib.
MESA_DIR="$ROOT_DIR/build/mesa-b1nix/$TRIPLET/install/lib"
GLAPI_A="$MESA_DIR/libglapi_static.a"
[ -f "$GLAPI_A" ] || { echo "build-skia-shared-deps: missing $GLAPI_A (run build-mesa.sh)" >&2; exit 1; }
"$LLD" -shared -m elf_x86_64 --hash-style=both -soname libGLESv2.so \
  --whole-archive "$GLAPI_A" --no-whole-archive \
  -L "$CROSSL" -lc -lm \
  --allow-shlib-undefined \
  -o "$UB/libGLESv2.so"

# --- 5) libEGL.so — b1nix EGL impl over Mesa OSMesa (for Dawn dlopen) -------
# Contains b1egl_mesa.c (full EGL 1.4) + egl_proc_resolver.c (eglGetProcAddress
# via dlsym, resolves GL functions from libGLESv2.so / libglapi).
EGL_MESA_C="$ROOT_DIR/userspace/libegl/b1egl_mesa.c"
EGL_RESOLVER_C="$ROOT_DIR/userspace/libegl/egl_proc_resolver.c"
[ -f "$EGL_MESA_C" ] || { echo "build-skia-shared-deps: missing $EGL_MESA_C" >&2; exit 1; }
[ -f "$EGL_RESOLVER_C" ] || { echo "build-skia-shared-deps: missing $EGL_RESOLVER_C" >&2; exit 1; }

WRAPPER="$ROOT_DIR/tools/ports/b1nix-cross-cc.sh"
EGL_OBJ="/tmp/b1egl_mesa_$$.o"
EGL_RES_OBJ="/tmp/b1egl_resolver_$$.o"
MESA_INC="$ROOT_DIR/build/mesa-b1nix/$TRIPLET/install/include"
B1GUI_INC="$ROOT_DIR/userspace/include"

"$WRAPPER" -O2 -I "$MESA_INC" -I "$B1GUI_INC" -c "$EGL_MESA_C" -o "$EGL_OBJ"
"$WRAPPER" -O2 -c "$EGL_RESOLVER_C" -o "$EGL_RES_OBJ"

# Link libEGL.so: just EGL entry points + OSMesa stubs.
# GL functions come from libGLESv2.so (loaded separately by Dawn).
# Mesa OSMesa functions (OSMesaCreateContext etc.) are needed at runtime
# but we provide them as weak stubs — the real Mesa stack is in the smoke binary.
"$LLD" -shared -m elf_x86_64 --hash-style=both -soname libEGL.so \
  "$EGL_OBJ" "$EGL_RES_OBJ" \
  -L "$UB" -L "$CROSSL" -lGLESv2 -lc -lm -ldl \
  --allow-shlib-undefined \
  -o "$UB/libEGL.so"

rm -f "$EGL_OBJ" "$EGL_RES_OBJ"

echo "build-skia-shared-deps: staged libskia/libraw_ptr/libfontconfig/libb1gui/libGLESv2/libEGL in $UB" >&2
