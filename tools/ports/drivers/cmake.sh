#!/bin/sh
# tools/ports/drivers/cmake.sh
#
# Generic driver for the CMake cross-compiled ports (brotli, litehtml, libharu,
# libjxl): fetch the source, stage the b1nix libc into the cross sysroot, write a
# b1nix CMake toolchain file, configure + build the requested targets with the
# b1nix cross gcc/g++, then copy headers and static archives. ccache is wired in
# through CMAKE_<LANG>_COMPILER_LAUNCHER (byte-identical output, faster rebuilds).
# Prints the install dir as its only stdout line.
#
# Per-port manifest (tools/ports/build-<port>.sh) sets the CMAKE_* variables and
# may define hooks:
#   port_fetch          override the default fetch (submodules / multi-tarball).
#                       Must leave the extracted tree at $SRC_DIR.
#   port_pre_configure  run after fetch + sysroot staging, before cmake configure.
#   port_install        copy headers + archives (use cmake_copy_archive). Required
#                       unless CMAKE_ARCHIVES + CMAKE_HEADER_DIRS are set.
#
# Manifest variables -------------------------------------------------------
#   CMAKE_NAME          port id; build dir = build/$CMAKE_NAME-b1nix/$TRIPLET
#   CMAKE_URL/CMAKE_TARBALL/CMAKE_SRCNAME/CMAKE_SENTINEL   default fetch inputs
#   CMAKE_SYSTEM_NAME   default "Linux" (use "Generic" for bare ports like libharu)
#   CMAKE_NEED_CXX      "1" → set CMAKE_CXX_COMPILER in the toolchain file
#   CMAKE_TC_EXTRA      extra raw lines appended to the toolchain file
#   CMAKE_ARGS         extra -D configure args (newline/space separated)
#   CMAKE_TARGETS      cmake --build --target list
#   CMAKE_JOBS         parallel jobs (default 4)
#   CMAKE_ARCHIVES     (default install) lib*.a names to find+copy into install/lib
#   CMAKE_HEADER_DIRS  (default install) "src->reldir" header dirs to cp -R

. "$ROOT_DIR/tools/ports/drivers/common.sh"

: "${CMAKE_NAME:?manifest must set CMAKE_NAME}"
CMAKE_SYSTEM_NAME="${CMAKE_SYSTEM_NAME:-Linux}"
CMAKE_JOBS="${CMAKE_JOBS:-${JOBS:-4}}"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/${CMAKE_SRCNAME:-$CMAKE_NAME}"
BUILD_DIR="$ROOT_DIR/build/$CMAKE_NAME-b1nix/$B1NIX_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$SRC_PARENT" "$BUILD_DIR/cmake" "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

# --- fetch ----------------------------------------------------------------
if command -v port_fetch >/dev/null 2>&1; then
  port_fetch
else
  : "${CMAKE_URL:?manifest must set CMAKE_URL or define port_fetch}"
  : "${CMAKE_TARBALL:?manifest must set CMAKE_TARBALL or define port_fetch}"
  port_fetch_tarball "$CMAKE_URL" "$SRC_PARENT/$CMAKE_TARBALL" "$SRC_PARENT" \
    "${CMAKE_SENTINEL:+$SRC_DIR/$CMAKE_SENTINEL}"
fi
if [ -n "${PATCHES:-}" ]; then
  # shellcheck disable=SC2086
  port_apply_patches "$SRC_DIR" $PATCHES
fi

# --- cross toolchain ------------------------------------------------------
CROSS="$(dirname "$(command -v "$B1NIX_TRIPLET-gcc" 2>/dev/null || echo "$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/bin/$B1NIX_TRIPLET-gcc")")"
GCC="$CROSS/$B1NIX_TRIPLET-gcc"
GXX="$CROSS/$B1NIX_TRIPLET-g++"
SYSROOT="$("$GCC" -print-sysroot 2>/dev/null)"
[ -n "$SYSROOT" ] || SYSROOT="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross/$B1NIX_TRIPLET"

# Resolve C++ frontend: clang++ (default) or g++ (legacy fallback)
CXX_FRONTEND="${B1NIX_CXX_FRONTEND:-clang}"
case "$CXX_FRONTEND" in
  gcc)
    USE_CXX="$GXX"
    ;;
  clang|*)
    USE_CXX="${B1NIX_CLANGXX:-$(command -v /opt/homebrew/opt/llvm/bin/clang++ 2>/dev/null || command -v clang++ 2>/dev/null || echo "$GXX")}"
    ;;
esac

# Stage the b1nix libc/libstdc++ into the cross sysroot (idempotent). Skip with
# CMAKE_STAGE_CXX=0 for the few ports that historically did not do this.
if [ "${CMAKE_STAGE_CXX:-1}" = "1" ]; then
  "$ROOT_DIR/tools/toolchain/enable-cxx-toolchain.sh" "$B1NIX_TRIPLET" 1>&2 || true
fi

# port_pre_configure runs before the toolchain file is written so it can resolve
# deps and extend CMAKE_TC_EXTRA / CMAKE_FIND_ROOT_EXTRA / CMAKE_ARGS. Has
# $SRC_DIR $BUILD_DIR $INSTALL_DIR $SYSROOT $GCC $GXX.
if command -v port_pre_configure >/dev/null 2>&1; then port_pre_configure; fi

# --- b1nix CMake toolchain file -------------------------------------------
# cmake 4.3+ sometimes ignores CMAKE_CXX_COMPILER from toolchain files for
# C++ projects, falling back to the host compiler. For C++ ports, set
# CMAKE_SKIP_TOOLCHAIN=1 in the manifest to bypass the toolchain file and
# use CC/CXX env vars + cmake args only (the reliable cmake 4 path).
TC="$BUILD_DIR/b1nix-toolchain.cmake"
{
  echo "set(CMAKE_SYSTEM_NAME $CMAKE_SYSTEM_NAME)"
  echo "set(CMAKE_SYSTEM_PROCESSOR $B1NIX_GCC_ARCH)"
  echo "set(CMAKE_C_COMPILER \"$GCC\")"
  echo "set(CMAKE_CXX_COMPILER \"$USE_CXX\")"
  echo "set(CMAKE_SYSROOT \"$SYSROOT\")"
  echo "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)"
  echo "set(CMAKE_FIND_ROOT_PATH \"$SYSROOT${CMAKE_FIND_ROOT_EXTRA:+;$CMAKE_FIND_ROOT_EXTRA}\")"
  echo "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)"
  echo "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)"
  echo "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)"
  printf '%s\n' "${CMAKE_TC_EXTRA:-}"
} > "$TC"

# --- configure + build ----------------------------------------------------
CCACHE_ARGS=""
if [ -n "$(port_ccache_prefix)" ]; then
  CCACHE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
fi

# Export CC/CXX — cmake 4 sometimes ignores CMAKE_CXX_COMPILER from the toolchain
# file for C++ projects. Env vars are the reliable fallback.
export CC="$GCC"
export CXX="$USE_CXX"

# When CMAKE_SKIP_TOOLCHAIN=1, pass all cross-compile settings as cmake args
# instead of using the toolchain file. Required for cmake 4 + C++ ports.
TC_ARGS=""
if [ "${CMAKE_SKIP_TOOLCHAIN:-0}" = "1" ]; then
  TC_ARGS="-DCMAKE_SYSTEM_NAME=$CMAKE_SYSTEM_NAME \
    -DCMAKE_SYSTEM_PROCESSOR=$B1NIX_GCC_ARCH \
    -DCMAKE_CROSSCOMPILING=TRUE \
    -DCMAKE_SYSROOT=$SYSROOT"
  TC=""
else
  TC="-DCMAKE_TOOLCHAIN_FILE=$TC"
fi

cd "$BUILD_DIR/cmake"
# shellcheck disable=SC2086
cmake "$SRC_DIR" \
  $TC \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  $CCACHE_ARGS \
  ${CMAKE_ARGS:-} 1>&2

# shellcheck disable=SC2086
cmake --build . --target ${CMAKE_TARGETS:-all} -j"$CMAKE_JOBS" 1>&2

# --- install --------------------------------------------------------------
# Helper for manifests: find a built archive by name and copy it into install/lib
# (optionally renamed). cmake_copy_archive <name.a> [dest-name.a]
cmake_copy_archive() {
  _f="$(find "$BUILD_DIR/cmake" -name "$1" 2>/dev/null | head -1)"
  [ -n "$_f" ] && cp "$_f" "$INSTALL_DIR/lib/${2:-$1}"
}

if command -v port_install >/dev/null 2>&1; then
  port_install
else
  for _hd in ${CMAKE_HEADER_DIRS:-}; do
    _src="${_hd%%->*}"; _dst="${_hd##*->}"
    mkdir -p "$INSTALL_DIR/include/$(dirname "$_dst")"
    cp -R "$SRC_DIR/$_src" "$INSTALL_DIR/include/$_dst"
  done
  for _a in ${CMAKE_ARCHIVES:-}; do cmake_copy_archive "$_a"; done
fi

echo "$INSTALL_DIR"
