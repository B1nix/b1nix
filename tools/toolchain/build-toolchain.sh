#!/usr/bin/env bash
# tools/toolchain/build-toolchain.sh - Builds the cross GCC & Binutils for the b1nix target
# selected by B1NIX_ARCH (x86 -> i686-b1nix, x86_64 -> x86_64-b1nix; default x86_64)
# Works on: Arch Linux / WSL and macOS (with Homebrew)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Per-architecture build identity: resolves B1NIX_ARCH -> B1NIX_TRIPLET,
# shared TOOLCHAIN_SRC_DIR, and per-triplet TOOLCHAIN_BUILD_HOME / B1NIX_ROOTFS
# (see tools/toolchain/env.sh).
. "$PROJECT_DIR/tools/toolchain/env.sh"
TARGET="$B1NIX_TRIPLET"

BINUTILS_VER="2.41"
GCC_VER="13.2.0"

BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"

# ── Platform detection ────────────────────────────────────────────────────────
OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
    NPROC=$(sysctl -n hw.ncpu)
    BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
    GMP_FLAG="--with-gmp=$BREW_PREFIX"
    MPFR_FLAG="--with-mpfr=$BREW_PREFIX"
    MPC_FLAG="--with-mpc=$BREW_PREFIX"
    SED_INPLACE() { sed -i '' "$@"; }
else
    NPROC=$(nproc)
    GMP_FLAG=""
    MPFR_FLAG=""
    MPC_FLAG=""
    SED_INPLACE() { sed -i "$@"; }
fi

# ── Shared source tree + per-triplet build/output directories ────────────────
# TOOLCHAIN_SRC_DIR holds one patched binutils/gcc source tree shared by every
# target. TOOLCHAIN_BUILD_HOME remains per triplet, because configure output,
# object files, sysroot symlink, and installed cross compilers are target-
# specific (GCC is --disable-multilib).
BUILD_HOME="$TOOLCHAIN_BUILD_HOME"
echo "Building $TARGET cross toolchain in $BUILD_HOME"

SRC_DIR="$TOOLCHAIN_SRC_DIR"
WORK_DIR="$BUILD_HOME/build"
PREFIX="$BUILD_HOME/cross"
# Binutils/GCC configure scripts break if sysroot contains spaces; the
# per-triplet sysroot is a symlink to the matching build/<arch>/rootfs.
SYSROOT="$BUILD_HOME/sysroot"

mkdir -p "$SRC_DIR" "$WORK_DIR" "$PREFIX"

# Sysroot symlink → the per-arch userspace rootfs (build/<arch>/rootfs).
mkdir -p "$B1NIX_ROOTFS"
ln -sfn "$B1NIX_ROOTFS" "$SYSROOT" 2>/dev/null || true

cd "$SRC_DIR"

# ── 1. Download sources (shared cache — fetched once, reused by every triplet) ─
mkdir -p "$TOOLCHAIN_DIST_DIR"
if [ ! -f "$TOOLCHAIN_DIST_DIR/binutils-${BINUTILS_VER}.tar.xz" ]; then
    echo "Downloading Binutils ${BINUTILS_VER}..."
    curl -L -o "$TOOLCHAIN_DIST_DIR/binutils-${BINUTILS_VER}.tar.xz" "$BINUTILS_URL"
fi

if [ ! -f "$TOOLCHAIN_DIST_DIR/gcc-${GCC_VER}.tar.xz" ]; then
    echo "Downloading GCC ${GCC_VER}..."
    curl -L -o "$TOOLCHAIN_DIST_DIR/gcc-${GCC_VER}.tar.xz" "$GCC_URL"
fi

# ── 2. Extract & Patch Binutils (shared source tree) ─────────────────────────
if [ ! -d "binutils-${BINUTILS_VER}" ]; then
    echo "Extracting Binutils..."
    tar -xf "$TOOLCHAIN_DIST_DIR/binutils-${BINUTILS_VER}.tar.xz"
    echo "Patching Binutils..."
    cd "binutils-${BINUTILS_VER}"
    patch -p1 < "$PROJECT_DIR/tools/patches/binutils.patch"
    SED_INPLACE 's/| fiwix\*/| fiwix* | b1nix*/g' config.sub
    cd ..
fi

# ── 3. Extract & Patch GCC (shared source tree) ──────────────────────────────
if [ ! -d "gcc-${GCC_VER}" ]; then
    echo "Extracting GCC..."
    tar -xf "$TOOLCHAIN_DIST_DIR/gcc-${GCC_VER}.tar.xz"
    echo "Patching GCC (b1nix target)..."
    python3 "$PROJECT_DIR/tools/toolchain/patch-gcc.py" "$SRC_DIR/gcc-${GCC_VER}"
    cd "gcc-${GCC_VER}"
    SED_INPLACE 's/| fiwix\*/| fiwix* | b1nix*/g' config.sub
    if [ -f "libgcc/config.sub" ]; then
        SED_INPLACE 's/| fiwix\*/| fiwix* | b1nix*/g' libgcc/config.sub
    fi
    # Fix libcody char8_t issue: GCC 13 uses u8"" literals (become char8_t in
    # C++20) but libcody's S2C() expects const char[]. Strip the u8 prefix.
    echo "Patching libcody (char8_t fix for modern host compilers)..."
    for f in libcody/*.cc libcody/*.hh; do
        [ -f "$f" ] && perl -i -pe 's/\bu8"/"/g' "$f"
    done
    cd ..
fi

# ── 4. Build cross-binutils ───────────────────────────────────────────────────
cd "$WORK_DIR"
if [ ! -f "$PREFIX/bin/${TARGET}-ld" ]; then
    echo "Building Cross-Binutils..."
    rm -rf build-binutils
    mkdir -p build-binutils
    cd build-binutils
    "$SRC_DIR/binutils-${BINUTILS_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot="$SYSROOT" \
        --disable-nls \
        --disable-werror \
        --with-system-zlib \
        MAKEINFO=true
    make -j"$NPROC" MAKEINFO=true
    make install MAKEINFO=true
    cd ..
fi

# Use LLVM lld as the cross linker (b1nix userspace is lld-based). GNU ld 2.41
# chokes ("failed to set dynamic section sizes: bad value") on the big shared,
# TLS-heavy links (libLLVM.so etc.); lld handles b1nix shared objects correctly.
# Keep the binutils ld as ld.bfd. Re-point on every run so a fresh binutils
# install doesn't silently restore the GNU ld.
# Make ld.lld available as a NAMED alias (ld.lld / <target>-ld.lld) so the rust
# + LLVM build can opt in with `-fuse-ld=lld`, but keep the DEFAULT `ld` as
# binutils ld.bfd. Pointing the default `ld` at lld broke the cross-gcc port
# links (e.g. NetSurf nsfb: `R_X86_64_32 against longjmp` — lld is stricter than
# bfd on absolute relocs in these non-PIC static EXEC links). lld is only needed
# for the big shared/TLS LLVM .so links, which select it explicitly.
LLD_BIN="$(command -v ld.lld || echo /usr/bin/ld.lld)"
if [ -x "$LLD_BIN" ]; then
    for bindir in "$PREFIX/$TARGET/bin" "$PREFIX/bin"; do
        [ -d "$bindir" ] || continue
        if [ "$bindir" = "$PREFIX/$TARGET/bin" ]; then
            [ -e "$bindir/ld.bfd" ] || cp -P "$bindir/ld" "$bindir/ld.bfd" 2>/dev/null || true
            ln -sf "$LLD_BIN" "$bindir/ld.lld"
        else
            ln -sf "$LLD_BIN" "$bindir/${TARGET}-ld.lld"
        fi
    done
fi

# ── 4b. Stage the b1nix libc headers into the sysroot ───────────────────────
# cross-GCC is configured with --enable-threads=posix, so libgcc's gthr-posix
# (#include <pthread.h>) needs the b1nix headers visible in the sysroot. They
# are produced by the clang-built userspace libc (no cross-GCC dependency), so
# stage them now — without this, all-target-libgcc dies with
# "pthread.h: No such file or directory".
if [ ! -f "$B1NIX_ROOTFS/include/pthread.h" ]; then
    echo "Staging b1nix libc headers into sysroot ($B1NIX_ROOTFS)..."
    make -C "$PROJECT_DIR/userspace" B1NIX_ARCH="$B1NIX_ARCH" install-headers-libs
fi

# Mirror the b1nix libc headers into the cross-toolchain's own include dirs.
# install-headers-libs only populates B1NIX_ROOTFS; the cross g++ resolves
# <stdint.h>/<math.h> through its gcc-wrapper which #include_next's
# $PREFIX/$TARGET/include and the sysroot. Without this sync those paths keep a
# stale copy (e.g. an incomplete C99 <stdint.h>/<math.h>), so libstdc++'s C99
# probes fail and disable <random>/std::isinf for every C++ port. Copy on every
# run so header fixes always reach the compiler.
for _incdst in "$PREFIX/$TARGET/include" "$SYSROOT/usr/include" "$SYSROOT/include"; do
    [ -d "$_incdst" ] || mkdir -p "$_incdst" 2>/dev/null || continue
    cp -f "$PROJECT_DIR/userspace/include/"*.h "$_incdst/" 2>/dev/null || true
    cp -rf "$PROJECT_DIR/userspace/include/"*/ "$_incdst/" 2>/dev/null || true
done

# Empty stub archives for libs that b1nix folds into libc (pthread/rt/dl are all
# in libc, musl-style). Software and GCC target libs link `-lpthread`/`-lrt`/
# `-ldl`; provide empty archives so those links resolve instead of failing.
for _stubdir in "$SYSROOT/usr/lib" "$SYSROOT/lib" "$PREFIX/$TARGET/lib"; do
    [ -d "$_stubdir" ] || mkdir -p "$_stubdir" 2>/dev/null || continue
    for _stub in libpthread librt libdl; do
        [ -f "$_stubdir/$_stub.a" ] || "$PREFIX/bin/${TARGET}-ar" rcs "$_stubdir/$_stub.a" 2>/dev/null || true
    done
done

# ── 5. Build cross-GCC ───────────────────────────────────────────────────────
if [ ! -f "$PREFIX/bin/${TARGET}-gcc" ]; then
    echo "Building Cross-GCC (bootstrap)..."
    rm -rf build-gcc
    mkdir -p build-gcc
    cd build-gcc
    "$SRC_DIR/gcc-${GCC_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot="$SYSROOT" \
        --disable-nls \
        --enable-languages=c,c++ \
        --without-headers \
        --enable-shared \
        --disable-multilib \
        --enable-threads=posix \
        --disable-libgomp \
        --disable-libmudflap \
        --disable-libssp \
        --disable-libquadmath \
        --with-newlib \
        --with-system-zlib \
        --with-isl \
        ${GMP_FLAG:+"$GMP_FLAG"} \
        ${MPFR_FLAG:+"$MPFR_FLAG"} \
        ${MPC_FLAG:+"$MPC_FLAG"} \
        MAKEINFO=true
    make -j"$NPROC" all-gcc all-target-libgcc MAKEINFO=true
    make install-gcc install-target-libgcc MAKEINFO=true
    cd ..
fi

# GCC's fixincludes emits a standalone include-fixed/stdlib.h that SHADOWS the
# complete b1nix <stdlib.h> with an incomplete copy (missing C99 lldiv/atoll/
# strtold/_Exit), which breaks libstdc++ <cstdlib> (`using ::strtold;`) for any
# C++ port (LLVM/rustc). b1nix headers are ours and complete — drop the bogus
# shadow so the real header is used.
rm -f "$PREFIX"/lib/gcc/"$TARGET"/*/include-fixed/stdlib.h 2>/dev/null || true

# ── 6. Build target C++ standard library ──────────────────────────────────────
# The native GCC port (tools/toolchain/build-native-toolchain.sh) cross-compiles GCC's own
# C++ sources (libcpp, gcc/*.cc) for x86_64-b1nix. Those include <new>, <vector>,
# etc. and link against libstdc++. Without a target libstdc++ the native build
# fails at libcpp with: "fatal error: new: No such file or directory".
#
# wchar_t: libstdc++'s configure auto-detects wide-char support by compiling a
# probe against the sysroot <wchar.h>/<wctype.h> (the full ::wcs*/::wmem*/wide-
# stdio set + ::wcstold/::wcstoll/::wcstoull + ::iswblank). The b1nix headers
# staged in step 4b declare all of them, so _GLIBCXX_USE_WCHAR_T is set to 1 and
# std::wcslen and the whole std:: wide-char family are available (required by the
# Chromium port — abseil str_format). If those probes ever fail again, the cause
# is a missing prototype in userspace/include/wchar.h or wctype.h, NOT this
# script. To force a re-detect on an EXISTING build tree, delete
# build-gcc/$TARGET/libstdc++-v3/config.cache, run ./config.status --recheck
# then ./config.status in that dir, and rebuild all-target-libstdc++-v3.
if [ ! -f "$PREFIX/$TARGET/lib/libstdc++.a" ]; then
    echo "Building target libstdc++-v3 for ${TARGET}..."
    if [ ! -f build-gcc/Makefile ]; then
        rm -rf build-gcc
        mkdir -p build-gcc
        cd build-gcc
        "$SRC_DIR/gcc-${GCC_VER}/configure" \
            --target="$TARGET" \
            --prefix="$PREFIX" \
            --with-sysroot="$SYSROOT" \
            --disable-nls \
            --enable-languages=c,c++ \
            --without-headers \
            --enable-shared \
            --disable-multilib \
            --enable-threads=posix \
            --disable-libgomp \
            --disable-libmudflap \
            --disable-libssp \
            --disable-libquadmath \
            --with-newlib \
            --with-system-zlib \
            --with-isl \
            ${GMP_FLAG:+"$GMP_FLAG"} \
            ${MPFR_FLAG:+"$MPFR_FLAG"} \
            ${MPC_FLAG:+"$MPC_FLAG"} \
            MAKEINFO=true
        cd ..
    fi
    cd build-gcc
    # Build libstdc++ with -fPIC. b1nix has no default-PIE, and libstdc++'s own
    # configure forces enable_shared=no on this newlib/elf target, so the static
    # libstdc++.a/libsupc++.a would be non-PIC and unusable in shared objects
    # (libLLVM.so, any C++ .so). -fPIC keeps the static archives PIC-foldable.
    make -j"$NPROC" all-target-libstdc++-v3 MAKEINFO=true \
        CXXFLAGS_FOR_TARGET="-g -O2 -fPIC" CFLAGS_FOR_TARGET="-g -O2 -fPIC"
    make install-target-libstdc++-v3 MAKEINFO=true
    cd ..
fi

# Alias the gcc unwinder as libunwind.a. Rust's `unwind` crate links `-lunwind`
# (the LLVM libunwind name) on musl + crt-static targets; b1nix ships gcc's
# unwinder (libgcc_eh.a) with the identical `_Unwind_*` ABI but a different name.
# Without this alias the final rustc link fails on undefined `_Unwind_*`.
_eh="$("$PREFIX/bin/${TARGET}-gcc" -print-file-name=libgcc_eh.a 2>/dev/null)"
if [ -f "$_eh" ]; then
    for _ud in "$SYSROOT/usr/lib" "$SYSROOT/lib" "$PREFIX/$TARGET/lib"; do
        [ -d "$_ud" ] && cp -f "$_eh" "$_ud/libunwind.a"
    done
fi

# Fold the EH unwinder objects (_Unwind_*) BACK INTO libgcc.a. Building GCC
# --enable-shared (needed for the rust libLLVM.so) splits the unwinder out of
# libgcc.a into libgcc_eh.a/libgcc_s.so. But b1nix's userspace C++ ports link
# the static libgcc via `-print-libgcc-file-name` (= libgcc.a) and DON'T add
# -lgcc_eh, so libstdc++'s __gxx_personality_v0 then has undefined `_Unwind_*`.
# Merging libgcc_eh.a's objects into libgcc.a restores the self-contained
# --disable-shared layout those links assume (EH members are only pulled when
# referenced, so plain C links are unaffected).
_libgcc="$("$PREFIX/bin/${TARGET}-gcc" -print-libgcc-file-name 2>/dev/null)"
_ar="$PREFIX/bin/${TARGET}-ar"
if [ -f "$_libgcc" ] && [ -f "$_eh" ] && [ -x "$_ar" ]; then
    _tmp_eh="$(mktemp -d)"
    ( cd "$_tmp_eh" && "$_ar" x "$_eh" )
    "$_ar" rs "$_libgcc" "$_tmp_eh"/*.o 2>/dev/null || true
    rm -rf "$_tmp_eh"
fi

echo ""
echo "Toolchain build completed successfully!"
echo "Cross compiler: $PREFIX/bin/${TARGET}-gcc"
