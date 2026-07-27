#!/bin/sh
# Build Mesa (OSMesa + Gallium swrast/softpipe, software OpenGL, no LLVM) as
# shared libraries for the b1nix userspace ABI. Cross-built with meson +
# the b1nix LLVM/Clang toolchain. Prints the install dir; install/lib/*.so
# are linked dynamically by Skia dm, smoke tests, etc.
#
# With -Ddefault_library=shared, meson produces .so files directly.  libc/libc++
# symbols are resolved at runtime by b1nix's dynamic linker (--allow-shlib-undefined).

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
MESA_VERSION="${MESA_VERSION:-24.0.9}"
TARBALL="mesa-${MESA_VERSION}.tar.xz"
URL="https://archive.mesa3d.org/${TARBALL}"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

. "$ROOT_DIR/tools/toolchain/env.sh"

# ccache for faster rebuilds (byte-identical objects)
CCACHE=""
if [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1; then
  CCACHE="$(command -v ccache)"
fi

SRC_PARENT="$ROOT_DIR/build/src/mesa"
mkdir -p "$SRC_PARENT"
SRC_DIR="$SRC_PARENT/mesa-${MESA_VERSION}"
BUILD_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/mesa"
MESON_BUILD="$BUILD_DIR/meson"
INSTALL_DIR="$BUILD_DIR/install"
CROSS="$(ls -d "$ROOT_DIR/build/$B1NIX_ARCH/toolchain/llvm/cross" "$ROOT_DIR/build/$B1NIX_ARCH/toolchain/$B1NIX_TRIPLET/cross" 2>/dev/null | head -1)"
CLANGXX_BIN="${B1NIX_CLANGXX:-$(command -v clang++ 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/clang++)}"

REAL_CC="${B1NIX_CC:-$(command -v clang 2>/dev/null || echo clang)}"
REAL_CXX="$CLANGXX_BIN"
STRIP_BIN="$(command -v llvm-strip 2>/dev/null || command -v strip 2>/dev/null || echo strip)"

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
acquire_lock() {
  WAIT_COUNT=0
  while ! mkdir "$LOCK" 2>/dev/null; do
    WAIT_COUNT=$((WAIT_COUNT + 1))
    if [ -f "$LOCK/pid" ]; then
      LOCK_PID="$(cat "$LOCK/pid" 2>/dev/null || echo "")"
      if [ -n "$LOCK_PID" ] && ! kill -0 "$LOCK_PID" 2>/dev/null; then
        echo "Removing stale build lock held by dead PID $LOCK_PID..." >&2
        rm -rf "$LOCK" 2>/dev/null || true
        continue
      fi
    elif [ -d "$LOCK" ]; then
      if [ "$WAIT_COUNT" -ge 3 ]; then
        echo "Removing lock directory missing PID file..." >&2
        rm -rf "$LOCK" 2>/dev/null || true
        continue
      fi
    fi
    sleep 1
  done
  echo "$$" > "$LOCK/pid" 2>/dev/null || true
}
acquire_lock
trap 'rm -rf "$LOCK" 2>/dev/null || true' EXIT INT TERM

# Toolchain prerequisites: musl supplies libc/crt. The legacy b1nix archive
# (libb1nix.a/crt0.o) is retired — Mesa must link against musl.
if [ ! -f "$ROOT_DIR/build/$B1NIX_ARCH/ports/musl/install/lib/libc.so" ]; then
  echo "build-mesa: musl not built — run tools/ports/build-musl.sh first" >&2
  exit 1
fi
make B1NIX_ARCH="$B1NIX_ARCH" LINK=musl -C "$ROOT_DIR/userspace" -s \
  "build/$B1NIX_ARCH/libb1gui.a" 1>&2

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/$TARBALL"
  [ -f "$tmp" ] || curl -L "$URL" -o "$tmp" 1>&2
  tar -xJf "$tmp" -C "$SRC_PARENT" 1>&2
fi

# Resolve C++ stdlib and CRT runtime. The unified runtimes build installs both
# libc++.a and libc++abi.a under libcxx-install/lib; these are always used.
CXX_STDLIB=libc++
LIBCXX_A="$ROOT_DIR/build/$B1NIX_ARCH/toolchain/$B1NIX_TRIPLET/llvm-runtimes-build/libcxx-install/lib/libc++.a"
LIBCXXABI_A="$ROOT_DIR/build/$B1NIX_ARCH/toolchain/$B1NIX_TRIPLET/llvm-runtimes-build/libcxx-install/lib/libc++abi.a"
LLVM_CRT_A="$ROOT_DIR/build/$B1NIX_ARCH/toolchain/$B1NIX_TRIPLET/llvm-runtimes-build/install/lib/libcompiler_rt.a"
LLVM_UNW_A=""

STDLIB_A="$LIBCXX_A"
STDLIB_ABI_A="$LIBCXXABI_A"
CRT_A="$LLVM_CRT_A"
UNW_A=""
CXX_CACHE_DISCRIM=", '-D__B1NIX_MESA_LIBCXX__=1'"

LB="$ROOT_DIR/userspace/build/$B1NIX_ARCH"

# meson cross file link args for SHARED libraries.
# Mesa .so files are PIC; they must NOT include crt0.o or libb1nix.a (non-PIC).
# libc/libc++ symbols are resolved at runtime by b1nix's dynamic linker, so we
# use --allow-shlib-undefined.  --hash-style=both ensures SysV DT_HASH for the
# b1nix kernel loader.
LIBCXX_SO_DIR="$ROOT_DIR/build/$B1NIX_ARCH/toolchain/$B1NIX_TRIPLET/llvm-runtimes-build/libcxx-install/lib"
# Shared-lib link args: no crt0.o, no libb1nix.a (non-PIC breaks -shared).
# Meson adds -shared automatically for .so targets.  libc symbols are resolved
# at runtime by b1nix's dynamic linker (--unresolved-symbols=ignore-all).
LIBCXX_SO_DIR="$ROOT_DIR/build/$B1NIX_ARCH/toolchain/$B1NIX_TRIPLET/llvm-runtimes-build/libcxx-install/lib"
MUSL_LIB="$ROOT_DIR/build/$B1NIX_ARCH/ports/musl/install/lib"
# Prefer musl-built runtime libs: the musl sysroot carries libc++/libc++abi
# compiled against musl headers, and the ports zlib is musl-clean. The legacy
# llvm-runtimes libc++.a and the cross-sysroot libz.a reference `errno` as a
# data symbol (old b1nix libc headers) — linking them into libOSMesa.so left
# an unresolvable UND errno that killed every Mesa consumer at load time.
ZLIB_PORT_LIB="$ROOT_DIR/build/$B1NIX_ARCH/ports/zlib/install/lib"
if [ -f "$MUSL_LIB/libc++.a" ]; then
  LIBCXX_SO_DIR="$MUSL_LIB"
fi
LINKARGS="'-fuse-ld=lld', '-nostdlib', '--sysroot=$CROSS', '-Wl,--hash-style=both', '-Wl,--unresolved-symbols=ignore-all', '-Wl,--allow-multiple-definition', '-L$LIBCXX_SO_DIR', '-L$ZLIB_PORT_LIB', '-L$MUSL_LIB', '-L$CROSS/$B1NIX_TRIPLET/lib', '-lc++', '-lc++abi'"

INI="$BUILD_DIR/cross.ini"
DEFS="'-Db1nix', '-D__b1nix__', '-D__linux__', '-DPATH_MAX=4096', '-DLLONG_MAX=9223372036854775807LL', '-DLLONG_MIN=(-LLONG_MAX-1LL)', '-DULLONG_MAX=18446744073709551615ULL', '-DHAVE_STRUCT_TIMESPEC', '-DUTIL_ARCH_LITTLE_ENDIAN=1', '-DUTIL_ARCH_BIG_ENDIAN=0', '-I$SRC_DIR/subprojects/zlib-1.3.1', '-UHAVE_QSORT_S', '-UHAVE_DLADDR', '-UHAVE_ARC4RANDOM_BUF', '-UHAVE_ARC4RANDOM', '-Dsecure_getenv=getenv'"
# M75: opt-in llvmpipe (MESA_LLVMPIPE=1). meson runs llvm-config on the host, so
# point it at the b1nix-LLVM wrapper; enable the shared libLLVM-22 and match its
# -fno-rtti (cpp_rtti=false). Default (unset) keeps the proven softpipe build.
LLVM_CONFIG_LINE=""
MESON_LLVM_OPTS="-Dllvm=disabled"
if [ "${MESA_LLVMPIPE:-0}" = "1" ]; then
  LLVM_CONFIG_LINE="llvm-config = '$ROOT_DIR/tools/ports/b1nix-llvm-config'"
  MESON_LLVM_OPTS="-Dllvm=enabled -Dshared-llvm=enabled -Dcpp_rtti=false"
  # Separate meson AND install dirs so the llvmpipe variant never clobbers the
  # softpipe install/lib (the default m52_osmesa demo links those archives, which
  # must stay free of undefined LLVM C-API symbols).
  MESON_BUILD="$BUILD_DIR/meson-llvmpipe"
  INSTALL_DIR="$BUILD_DIR/install-llvmpipe"
  mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"
fi
cat > "$INI" <<EOF
[binaries]
c = ['$ROOT_DIR/tools/toolchain/bin/b1nix-mesa-cc', '$REAL_CC']
cpp = ['$ROOT_DIR/tools/toolchain/bin/b1nix-mesa-cc', '$REAL_CXX']
ar = '$AR_BIN'
strip = '$STRIP_BIN'
pkg-config = 'false'
$LLVM_CONFIG_LINE
[host_machine]
system = 'linux'
cpu_family = '$MFAM'
cpu = '$MCPU'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = [$DEFS]
cpp_args = [$DEFS$CXX_CACHE_DISCRIM]
c_link_args = [$LINKARGS]
cpp_link_args = [$LINKARGS]
EOF

# On i686, Mesa auto-enables its x86 (i386) assembly GL dispatch (USE_X86_ASM,
# meson.build's cpu_family=='x86' branch — x86_64 takes a different branch and is
# unaffected). Its glapi entry-point reloc path references _x86_get_dispatch,
# which the static-glapi (shared-glapi=disabled) build does not provide -> link
# error. Mesa 24 has no -Dasm option, so neutralise that x86-only block in
# meson.build; the portable C dispatch is fine for a software port. Idempotent.
if sed --version >/dev/null 2>&1; then SEDI() { sed -i "$@"; }
else SEDI() { sed -i '' "$@"; } fi
if [ "$B1NIX_ARCH" = "x86" ]; then
  SEDI "s/    with_asm_arch = 'x86'/    with_asm_arch = '' # b1nix: no i386 asm dispatch/" "$SRC_DIR/meson.build"
  SEDI "/    pre_args += \['-DUSE_X86_ASM'\]/d" "$SRC_DIR/meson.build"
fi
# x86_64: the generated glapi x86-64 asm dispatch (glapi_x86-64.S) accesses
# _glapi_tls_Dispatch via @GOTTPOFF — an initial-exec TLS reloc (R_X86_64_TPOFF64)
# that b1nix's musl ld.so cannot satisfy for the dynamically-loaded libOSMesa.so
# (load fails, m52-osmesa exits 127 before main). Neutralise the x86_64 asm so
# glapi uses portable C dispatch (general-dynamic TLS) — fine for a software port.
if [ "$B1NIX_ARCH" = "x86_64" ]; then
  SEDI "s/    with_asm_arch = 'x86_64'/    with_asm_arch = '' # b1nix: no x86_64 asm dispatch (IE-TLS)/" "$SRC_DIR/meson.build"
  SEDI "/    pre_args += \['-DUSE_X86_64_ASM'\]/d" "$SRC_DIR/meson.build"
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
  # NOTE: do NOT export CC_LD="$CCACHE" — CC_LD is meson's *linker*, and ccache
  # is not a linker (newer meson hard-errors: "Unsupported linker ... ccache").
  # ccache wrapping, if wanted, belongs on the compiler command in cross.ini.
  ( cd "$SRC_DIR" && meson setup "$MESON_BUILD" --cross-file "$INI" \
      -Dgallium-drivers=swrast,virgl -Dvulkan-drivers= $MESON_LLVM_OPTS -Dosmesa=true \
      -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= -Dopengl=true \
      -Dgles1=disabled -Dgles2=disabled -Dshared-glapi=disabled \
      -Ddefault_library=both -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
      -Dvalgrind=disabled -Dbuild-tests=false -Dshader-cache=disabled 1>&2 )
  # Fix: meson's has_function probe detects qsort_s from the host compiler,
  # but b1nix libc doesn't have it. Replace -D with -U in the generated ninja.
  if [ -f "$MESON_BUILD/build.ninja" ]; then
    sed -i 's/-DHAVE_QSORT_S/-UHAVE_QSORT_S/g' "$MESON_BUILD/build.ninja"
  fi
fi

# expat (Mesa's vendored XML subproject): meson's has_function probe false-
# positively defines HAVE_ARC4RANDOM / HAVE_ARC4RANDOM_BUF in the generated
# expat_config.h (the host toolchain resolves the symbol even though the musl
# target libc has no arc4random at all). xmlparse.c then calls the undeclared
# arc4random_buf and clang errors on the implicit declaration. Editing the
# generated header does not stick — ninja auto-reconfigures and meson rewrites
# it with the bad defines. Instead #undef both right after the expat_config.h
# include in the (stable, unpacked) subproject source so expat takes its
# getrandom path (HAVE_GETRANDOM is set; musl provides getrandom). Idempotent:
# skip if the marker is already present.
EXPAT_XMLPARSE="$SRC_DIR/subprojects/expat-2.5.0/lib/xmlparse.c"
if [ -f "$EXPAT_XMLPARSE" ]; then
  perl -0pi -e 'unless (/b1nix: musl has no arc4random/) {
    s/(#include <expat_config\.h>\n)/$1\n\/* b1nix: musl has no arc4random; use the getrandom fallback. *\/\n#undef HAVE_ARC4RANDOM_BUF\n#undef HAVE_ARC4RANDOM\n/;
  }' "$EXPAT_XMLPARSE"
fi

# libc++ runtime errno fix: meson's C++ compiler probe records the *legacy*
# (old-b1nix-libc) libc++ under toolchain_build/.../llvm-runtimes-build/libcxx-
# install/lib as a link input for every C++ target. Those legacy libc++.a/
# libc++abi.a reference `errno` as a DATA symbol (built against the old b1nix
# errno.h `extern int errno`), which musl's libc.so does not export — so
# libOSMesa.so ends up with an unresolvable UND errno and every Mesa consumer
# dies at load ("Error relocating libOSMesa.so.8: errno: symbol not found").
# The path is baked into build.ninja by meson and regenerated on every ninja
# auto-reconfigure, so patching build.ninja does not stick. Instead overwrite the
# legacy archives in place with the musl-built ones (same target, compiled against
# musl headers → errno is the __errno_location macro, no data symbol). The legacy
# non-musl libc++ is dead under the musl migration; env.sh already routes every
# other consumer to llvm-runtimes-build-musl. Idempotent.
LEGACY_LIBCXX="$ROOT_DIR/build/$B1NIX_ARCH/toolchain/$B1NIX_TRIPLET/llvm-runtimes-build/libcxx-install/lib"
MUSL_LIBCXX="$ROOT_DIR/build/$B1NIX_ARCH/toolchain/$B1NIX_TRIPLET/llvm-runtimes-build-musl/libcxx-install/lib"
if [ -f "$MUSL_LIBCXX/libc++.a" ] && [ -d "$LEGACY_LIBCXX" ]; then
  cp -f "$MUSL_LIBCXX/libc++.a"    "$LEGACY_LIBCXX/libc++.a"
  cp -f "$MUSL_LIBCXX/libc++abi.a" "$LEGACY_LIBCXX/libc++abi.a"
fi

# Build all reachable targets. With default_library=shared, meson produces
# .so files directly. --unresolved-symbols=ignore-all lets libc symbols
# be resolved at runtime by b1nix's dynamic linker.
( cd "$MESON_BUILD" && ninja -j"$(nproc 2>/dev/null || echo 4)" 1>&2 )

# Install shared libraries to install/lib/.
# With default_library=shared, meson produces .so files.  We install the key
# consumer-facing ones plus any internal deps they DT_NEEDED.
rm -f "$INSTALL_DIR/lib/"*.a "$INSTALL_DIR/lib/"*.so  # clean stale static libs
mkdir -p "$INSTALL_DIR/lib"
( cd "$MESON_BUILD"
  for so in $(find . -name "*.so" -o -name "*.so.*" | grep -v '\.debug' | grep -v '\.p$' | grep -v '\.p/'); do
    [ -f "$so" ] || continue
    name="$(basename "$so")"
    [ -L "$so" ] && continue  # skip symlinks, we'll create them below
    cp -f "$so" "$INSTALL_DIR/lib/$name"
  done
  # Create convenience symlinks: libfoo.so -> libfoo.so.0 (or whatever soname)
  cd "$INSTALL_DIR/lib"
  for so in *.so.*; do
    [ -f "$so" ] || continue
    base="$(echo "$so" | sed 's/\.so\..*/.so/')"
    ln -sfn "$so" "$base" 2>/dev/null || true
  done
)

# Also install the static archives that consumers may need for whole-archive
# linking (c11threads, glapi dispatch).  Repack thin archives if present.
( cd "$MESON_BUILD"
  for a in $(find . -name "*.a"); do
    name="$(basename "$a")"
    case "$name" in
      libglsl_standalone.a) continue ;;  # stub scaffolding — never install
    esac
    target="$INSTALL_DIR/lib/$name"
    if [ ! -f "$target" ] || [ "$a" -nt "$target" ]; then
      if [ "$(head -c 7 "$a")" = "!<thin>" ]; then
        rm -f "$target"
        "$AR_BIN" crs "$target" $("$AR_BIN" t "$a") 2>/dev/null || true
      else
        cp "$a" "$target" 2>/dev/null || true
      fi
    fi
  done ) || true

# The osmesa target glue (osmesa_create_screen): extract from the .so link objects.
for src_obj in "$MESON_BUILD"/src/gallium/targets/osmesa/libOSMesa.so.*.p/target.c.o \
               "$MESON_BUILD"/src/gallium/targets/osmesa/*.p/target.c.o; do
  if [ -f "$src_obj" ]; then
    target_obj="$INSTALL_DIR/lib/osmesa_target.o"
    if [ ! -f "$target_obj" ] || [ "$src_obj" -nt "$target_obj" ]; then
      cp "$src_obj" "$target_obj"
    fi
    break
  fi
done

cp -R "$SRC_DIR/include/GL" "$SRC_DIR/include/KHR" "$INSTALL_DIR/include/" 2>/dev/null || true

echo "$INSTALL_DIR"
