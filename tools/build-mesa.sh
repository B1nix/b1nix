#!/bin/sh
# Build Mesa (OSMesa + Gallium swrast/softpipe, software OpenGL, no LLVM) as a
# set of static libraries for the b1nix userspace ABI. Cross-built with meson +
# the b1nix GCC/libstdc++ toolchain (needs tools/enable-cxx-toolchain.sh, run
# here). Prints the install dir; install/lib/*.a + the osmesa target object are
# linked by the M52 OSMesa demo.
#
# Mesa is built static; its own meson archives are thin (reference .o by path),
# so they are repacked into relocatable thick archives in install/lib. The final
# libOSMesa.so target is intentionally skipped (b1nix is static-only); its glue
# (osmesa_create_screen) lives in install/lib/osmesa_target.o.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MESA_VERSION="${MESA_VERSION:-24.0.9}"
TARBALL="mesa-${MESA_VERSION}.tar.xz"
URL="https://archive.mesa3d.org/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain-env.sh"

SRC_PARENT="$ROOT_DIR/build/ports-src"
SRC_DIR="$SRC_PARENT/mesa-${MESA_VERSION}"
BUILD_DIR="$ROOT_DIR/build/mesa-b1nix/$B1NIX_TRIPLET"
MESON_BUILD="$BUILD_DIR/meson"
INSTALL_DIR="$BUILD_DIR/install"
CROSS="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/cross"
GXX="$CROSS/bin/$B1NIX_TRIPLET-g++"

if [ "$B1NIX_ARCH" = "x86" ]; then
  LDEMU="elf_i386"; MCPU="x86"; MFAM="x86"
else
  LDEMU="elf_x86_64"; MCPU="x86_64"; MFAM="x86_64"
fi

mkdir -p "$SRC_PARENT" "$BUILD_DIR" "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

# Toolchain prerequisites: b1nix libc/crt + C++-enabled cross toolchain.
make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o" 1>&2
"$ROOT_DIR/tools/enable-cxx-toolchain.sh" "$B1NIX_TRIPLET" 1>&2

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xJf "$tmp" -C "$SRC_PARENT" 1>&2
fi

LIBSTDCXX="$("$GXX" -print-file-name=libstdc++.a)"
LIBSUPCXX="$("$GXX" -print-file-name=libsupc++.a)"
LIBGCC="$("$GXX" -print-libgcc-file-name)"
LB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"

# meson cross file. The compiler is wrapped (tools/b1nix-mesa-cc) to strip
# -pthread, which the b1nix cross GCC has no spec for. PATH_MAX/LLONG_MAX are
# defined because some generated/flex C++ files use them without including the
# header that declares them. system='linux' + -D__linux__ make Mesa take its
# generic POSIX path (b1nix masquerades as Linux for ports).
INI="$BUILD_DIR/cross.ini"
DEFS="'-Db1nix', '-D__b1nix__', '-D__linux__', '-DPATH_MAX=4096', '-DLLONG_MAX=9223372036854775807LL', '-DLLONG_MIN=(-LLONG_MAX-1LL)', '-DULLONG_MAX=18446744073709551615ULL'"
LINKARGS="'-nostdlib', '-T', '$ROOT_DIR/userspace/linker-cxx.ld', '-Wl,--allow-multiple-definition', '$LB/crt/crt0.o', '-Wl,--start-group', '$LIBSTDCXX', '$LIBSUPCXX', '$LIBGCC', '-Wl,--whole-archive', '$LB/libb1nix.a', '-Wl,--no-whole-archive', '-Wl,--end-group'"
cat > "$INI" <<EOF
[binaries]
c = ['$ROOT_DIR/tools/b1nix-mesa-cc', '$CROSS/bin/$B1NIX_TRIPLET-gcc']
cpp = ['$ROOT_DIR/tools/b1nix-mesa-cc', '$CROSS/bin/$B1NIX_TRIPLET-g++']
ar = '$CROSS/bin/$B1NIX_TRIPLET-ar'
strip = '$CROSS/bin/$B1NIX_TRIPLET-strip'
pkg-config = 'false'
[host_machine]
system = 'linux'
cpu_family = '$MFAM'
cpu = '$MCPU'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = [$DEFS]
cpp_args = [$DEFS]
c_link_args = [$LINKARGS]
cpp_link_args = [$LINKARGS]
EOF

if [ ! -f "$MESON_BUILD/build.ninja" ]; then
  ( cd "$SRC_DIR" && meson setup "$MESON_BUILD" --cross-file "$INI" \
      -Dgallium-drivers=swrast -Dvulkan-drivers= -Dllvm=disabled -Dosmesa=true \
      -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= -Dopengl=true \
      -Dgles1=disabled -Dgles2=disabled -Dshared-glapi=disabled \
      -Ddefault_library=static -Dzstd=disabled -Dlibunwind=disabled \
      -Dvalgrind=disabled -Dbuild-tests=false -Dshader-cache=disabled 1>&2 )
fi

# Build the static libraries. The final libOSMesa.so link fails on purpose
# (static-only b1nix); every .a we need is produced before it, so ignore it.
( cd "$MESON_BUILD" && ninja 1>&2 ) || true

# Repack thin archives into relocatable thick archives in install/lib.
rm -f "$INSTALL_DIR"/lib/*.a
( cd "$MESON_BUILD"
  for a in $(find . -name "*.a"); do
    name="$(basename "$a")"
    if [ "$(head -c 7 "$a")" = "!<thin>" ]; then
      # shellcheck disable=SC2046
      "$AR_BIN" crs "$INSTALL_DIR/lib/$name" $("$AR_BIN" t "$a")
    else
      cp "$a" "$INSTALL_DIR/lib/$name"
    fi
  done )

# The osmesa target glue (osmesa_create_screen): only built as part of the .so.
cp "$MESON_BUILD"/src/gallium/targets/osmesa/libOSMesa.so.*.p/target.c.o \
  "$INSTALL_DIR/lib/osmesa_target.o"

cp -R "$SRC_DIR/include/GL" "$SRC_DIR/include/KHR" "$INSTALL_DIR/include/" 2>/dev/null || true

echo "$INSTALL_DIR"
