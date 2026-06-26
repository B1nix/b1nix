#!/bin/sh
# Build Mesa (OSMesa + Gallium swrast/softpipe, software OpenGL, no LLVM) as a
# set of static libraries for the b1nix userspace ABI. Cross-built with meson +
# the b1nix GCC/libstdc++ toolchain (needs tools/toolchain/enable-cxx-toolchain.sh, run
# here). Prints the install dir; install/lib/*.a + the osmesa target object are
# linked by the M52 OSMesa demo.
#
# Mesa is built static; its own meson archives are thin (reference .o by path),
# so they are repacked into relocatable thick archives in install/lib. The final
# libOSMesa.so target is intentionally skipped (b1nix is static-only); its glue
# (osmesa_create_screen) lives in install/lib/osmesa_target.o.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
MESA_VERSION="${MESA_VERSION:-24.0.9}"
TARBALL="mesa-${MESA_VERSION}.tar.xz"
URL="https://archive.mesa3d.org/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

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

# Serialize concurrent invocations. The Makefile builds several Mesa-dependent
# initramfs targets (m52_osmesa, m53_mesa_virgl, m52_glsl, m59_smoke) in
# parallel; each calls this script, which rewrites cross.ini and runs ninja in
# the shared meson dir. Parallel runs collide ("Some other Meson process is
# already using this build directory"). ponytail: mkdir is atomic on POSIX —
# spin until we own the lock, drop it on exit. First caller builds Mesa; the
# rest wait, then find it up to date.
LOCK="$BUILD_DIR/.build-lock"
while ! mkdir "$LOCK" 2>/dev/null; do sleep 1; done
trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT INT TERM

# Toolchain prerequisites: b1nix libc/crt + C++-enabled cross toolchain.
make B1NIX_ARCH="$B1NIX_ARCH" -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1nix.a" "build/$B1NIX_ARCH/crt/crt0.o" 1>&2
"$ROOT_DIR/tools/toolchain/enable-cxx-toolchain.sh" "$B1NIX_TRIPLET" 1>&2

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xJf "$tmp" -C "$SRC_PARENT" 1>&2
fi

LIBSTDCXX="$("$GXX" -print-file-name=libstdc++.a)"
LIBSUPCXX="$("$GXX" -print-file-name=libsupc++.a)"
LIBGCC="$("$GXX" -print-libgcc-file-name)"
LB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"

# meson cross file. The compiler is wrapped (tools/toolchain/bin/b1nix-mesa-cc) to strip
# -pthread, which the b1nix cross GCC has no spec for. PATH_MAX/LLONG_MAX are
# defined because some generated/flex C++ files use them without including the
# header that declares them. system='linux' + -D__linux__ make Mesa take its
# generic POSIX path (b1nix masquerades as Linux for ports).
INI="$BUILD_DIR/cross.ini"
DEFS="'-Db1nix', '-D__b1nix__', '-D__linux__', '-DPATH_MAX=4096', '-DLLONG_MAX=9223372036854775807LL', '-DLLONG_MIN=(-LLONG_MAX-1LL)', '-DULLONG_MAX=18446744073709551615ULL'"
LINKARGS="'-nostdlib', '-T', '$ROOT_DIR/userspace/linker-cxx.ld', '-Wl,--allow-multiple-definition', '$LB/crt/crt0.o', '-Wl,--start-group', '$LIBSTDCXX', '$LIBSUPCXX', '$LIBGCC', '-Wl,--whole-archive', '$LB/libb1nix.a', '-Wl,--no-whole-archive', '-Wl,--end-group'"
cat > "$INI" <<EOF
[binaries]
c = ['$ROOT_DIR/tools/toolchain/bin/b1nix-mesa-cc', '$CROSS/bin/$B1NIX_TRIPLET-gcc']
cpp = ['$ROOT_DIR/tools/toolchain/bin/b1nix-mesa-cc', '$CROSS/bin/$B1NIX_TRIPLET-g++']
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

# On i686, Mesa auto-enables its x86 (i386) assembly GL dispatch (USE_X86_ASM,
# meson.build's cpu_family=='x86' branch — x86_64 takes a different branch and is
# unaffected). Its glapi entry-point reloc path references _x86_get_dispatch,
# which the static-glapi (shared-glapi=disabled) build does not provide -> link
# error. Mesa 24 has no -Dasm option, so neutralise that x86-only block in
# meson.build; the portable C dispatch is fine for a software port. Idempotent.
if [ "$B1NIX_ARCH" = "x86" ]; then
  if sed --version >/dev/null 2>&1; then SEDI() { sed -i "$@"; }
  else SEDI() { sed -i '' "$@"; } fi
  SEDI "s/    with_asm_arch = 'x86'/    with_asm_arch = '' # b1nix: no i386 asm dispatch/" "$SRC_DIR/meson.build"
  SEDI "/    pre_args += \['-DUSE_X86_ASM'\]/d" "$SRC_DIR/meson.build"
fi

# b1nix Mesa source changes (e.g. the VirGL winsys for /dev/virtio-gpu, M53
# variant B) live in this repo, NOT in the extracted tarball, so they are
# reproducible on any host. Apply them here, idempotently:
#   tools/patches/mesa/*.patch     — unified diffs against the Mesa tree (-p1)
#   tools/patches/mesa/files/...   — whole b1nix-owned files copied into the tree
# Patches are skipped if already applied (a reverse dry-run succeeds); files are
# copied verbatim every run (a copy is itself idempotent).
PATCH_DIR="$ROOT_DIR/tools/patches/mesa"
if [ -d "$PATCH_DIR" ]; then
  for p in "$PATCH_DIR"/*.patch; do
    [ -e "$p" ] || continue
    if patch -p1 -d "$SRC_DIR" --dry-run -R <"$p" >/dev/null 2>&1; then
      echo "build-mesa: patch already applied: $(basename "$p")" 1>&2
    elif patch -p1 -d "$SRC_DIR" --dry-run <"$p" >/dev/null 2>&1; then
      patch -p1 -d "$SRC_DIR" <"$p" 1>&2
      echo "build-mesa: applied patch: $(basename "$p")" 1>&2
    else
      echo "build-mesa: ERROR: cannot apply $(basename "$p")" 1>&2
      exit 1
    fi
  done
  if [ -d "$PATCH_DIR/files" ]; then
    cp -R "$PATCH_DIR/files/." "$SRC_DIR/" 1>&2
    echo "build-mesa: installed b1nix Mesa files" 1>&2
  fi
fi

# Wire the gallium virgl driver to the b1nix winsys instead of the libdrm/vtest
# ones (b1nix has no libdrm), and drop the driver's vestigial dep_libdrm (it
# uses no drm symbols; its lone <libsync.h> resolves to Mesa's own src/util one).
# Idempotent SED edits (the substituted strings no longer match on re-runs).
if sed --version >/dev/null 2>&1; then SEDI() { sed -i "$@"; }
else SEDI() { sed -i '' "$@"; } fi
SEDI "s|subdir('winsys/virgl/drm')|subdir('winsys/virgl/b1nix')|" "$SRC_DIR/src/gallium/meson.build"
SEDI "/subdir('winsys\/virgl\/vtest')/d" "$SRC_DIR/src/gallium/meson.build"
SEDI "s|dependencies : \[dep_libdrm, idep_mesautil, idep_xmlconfig, idep_nir\],|dependencies : [idep_mesautil, idep_xmlconfig, idep_nir],|" "$SRC_DIR/src/gallium/drivers/virgl/meson.build"
SEDI "s|link_with : \[libvirgl, libvirgldrm, libvirglvtest\],|link_with : [libvirgl, libvirglb1nix],|" "$SRC_DIR/src/gallium/drivers/virgl/meson.build"
# The virgl driver's lone <libsync.h> (angle-bracket, from libdrm) — point it at
# Mesa's own src/util/libsync.h instead, since we dropped dep_libdrm.
SEDI 's|#include <libsync.h>|#include "util/libsync.h"|' "$SRC_DIR/src/gallium/drivers/virgl/virgl_context.c"
# Stub virgl_disk_cache_create: it uses util/build_id which is empty without
# HAVE_DL_ITERATE_PHDR (b1nix has none), and the shader/disk cache is disabled
# anyway. Idempotent (the stubbed body re-matches to the same stub).
perl -0pi -e 's/static void virgl_disk_cache_create\(struct virgl_screen \*screen\)\n\{.*?\n\}/static void virgl_disk_cache_create(struct virgl_screen *screen)\n{\n   screen->disk_cache = NULL; \/* b1nix: no build-id\/disk cache *\/\n}/s' "$SRC_DIR/src/gallium/drivers/virgl/virgl_screen.c"
# virgl_video.c uses MIN/MAX (from <sys/param.h> on Linux); b1nix lacks them.
# Inject portable defines after the first #include (idempotent: skip if present).
perl -0pi -e 'unless (/#define MIN\(a,\s*b\)/) { s/(#include [^\n]*\n)/$1#ifndef MIN\n#define MIN(a, b) ((a) < (b) ? (a) : (b))\n#endif\n#ifndef MAX\n#define MAX(a, b) ((a) > (b) ? (a) : (b))\n#endif\n/; }' "$SRC_DIR/src/gallium/drivers/virgl/virgl_video.c"

if [ ! -f "$MESON_BUILD/build.ninja" ]; then
  ( cd "$SRC_DIR" && meson setup "$MESON_BUILD" --cross-file "$INI" \
      -Dgallium-drivers=swrast,virgl -Dvulkan-drivers= -Dllvm=disabled -Dosmesa=true \
      -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= -Dopengl=true \
      -Dgles1=disabled -Dgles2=disabled -Dshared-glapi=disabled \
      -Ddefault_library=static -Dzstd=disabled -Dlibunwind=disabled \
      -Dvalgrind=disabled -Dbuild-tests=false -Dshader-cache=disabled 1>&2 )
fi

# Build the static libraries. The final libOSMesa.so link fails on purpose
# (static-only b1nix). Some driver archives (e.g. libvirgl.a) are scheduled
# AFTER that .so in ninja order, so a plain `ninja` would abort on the expected
# .so failure and never build them. `-k 0` keeps going past the failure and
# builds every reachable target; the `|| true` swallows the final nonzero exit.
( cd "$MESON_BUILD" && ninja -k 0 1>&2 ) || true

# Repack thin archives into relocatable thick archives in install/lib.
# Avoid unconditional rm -f to prevent race conditions when parallel demo builds
# try to link against these archives while another instance is repacking them.
( cd "$MESON_BUILD"
  for a in $(find . -name "*.a"); do
    name="$(basename "$a")"
    target="$INSTALL_DIR/lib/$name"
    if [ ! -f "$target" ] || [ "$a" -nt "$target" ]; then
      if [ "$(head -c 7 "$a")" = "!<thin>" ]; then
        rm -f "$target"
        # shellcheck disable=SC2046
        "$AR_BIN" crs "$target" $("$AR_BIN" t "$a")
      else
        cp "$a" "$target"
      fi
    fi
  done )

# The osmesa target glue (osmesa_create_screen): only built as part of the .so.
for src_obj in "$MESON_BUILD"/src/gallium/targets/osmesa/libOSMesa.so.*.p/target.c.o; do
  if [ -f "$src_obj" ]; then
    target_obj="$INSTALL_DIR/lib/osmesa_target.o"
    if [ ! -f "$target_obj" ] || [ "$src_obj" -nt "$target_obj" ]; then
      cp "$src_obj" "$target_obj"
    fi
  fi
done

cp -R "$SRC_DIR/include/GL" "$SRC_DIR/include/KHR" "$INSTALL_DIR/include/" 2>/dev/null || true

echo "$INSTALL_DIR"
