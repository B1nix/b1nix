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
  # Static build — create empty placeholders.
  #
  # With the host clang, as this used to do: the stub came out linked against
  # the host's glibc (DT_NEEDED libc.so.6) and shipped into the rootfs, where
  # nothing can load it. Placeholders are still b1nix binaries. The failure is
  # not swallowed either — a stub we cannot build is a build error, not a
  # silently missing file.
  echo "build-skia-shared-deps: no libskia.so (static build), creating placeholders" >&2
  # -nostdlib: a placeholder has nothing to import, so it needs no libc at all
  # and records no DT_NEEDED. Built for the b1nix target, not the host.
  _stub_dir=$(mktemp -d)
  echo "void __skia_placeholder(void) {}" > "$_stub_dir/skia_stub.c"
  echo "void __raw_ptr_placeholder(void) {}" > "$_stub_dir/raw_ptr_stub.c"
  clang --target=x86_64-b1nix -shared -nostdlib -fPIC \
    "$_stub_dir/skia_stub.c" -o "$UB/libskia.so"
  clang --target=x86_64-b1nix -shared -nostdlib -fPIC \
    "$_stub_dir/raw_ptr_stub.c" -o "$UB/libraw_ptr.so"
  rm -rf "$_stub_dir"
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

# A libGLESv2.so used to be manufactured here, by folding the Mesa port's
# libglapi_static.a into a shared object for Dawn to dlopen. Mesa comes from
# Alpine now (tools/packages/alpine-ports.map), and mesa-gles ships the real
# libGLESv2.so.2 — a shared library with those entry points in it, already on
# the image — so there is nothing left to fabricate.

