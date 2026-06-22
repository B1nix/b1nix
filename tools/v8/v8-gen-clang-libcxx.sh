#!/bin/sh
# Gen a V8 build with CLANG + Chromium's bundled libc++ (use_custom_libcxx) for
# b1nix — the C++23 path. GCC 13.2.0's libstdc++ lacks C++23 (std::from_range_t
# etc.) and the bundled libc++ won't compile under GCC; clang compiles libc++
# and supports C++23, so this is the toolchain Chromium actually wants.
#
# Differs from tools/v8-gen-clang.sh (clang + GCC libstdc++):
#   - use_custom_libcxx=true (libc++ instead of libstdc++)
#   - the clang cppflags drop the GCC libstdc++ -isystem header paths; libc++
#     provides the C++ headers (added by GN), b1nix libc provides the C headers
#   - Temporal + Rust ON (the V8-with-Rust config), reusing the cross-rust
#
# Output: out/b1nix-jit-clang-libcxx. Then:
#   ninja -C .../out/b1nix-jit-clang-libcxx d8
#   sh tools/v8/v8-link-d8.sh b1nix-jit-clang-libcxx   (libc++ in the link group)
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SK="$ROOT_DIR/build/toolchain_build/v8-skeleton"
GN="$SK/gn-src/out/gn"
[ -x "$GN" ] || { echo "gn missing — run: sh tools/v8/build-gn.sh"; exit 1; }
[ -d "$SK/v8/src" ] || { echo "v8 checkout missing at $SK/v8"; exit 1; }
sh "$ROOT_DIR/tools/patches/v8/apply.sh" "$SK/v8" || true

CROSS="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/cross"
GXX="$CROSS/bin/x86_64-b1nix-g++"
CLANGXX="${B1NIX_CLANGXX:-$(command -v clang++)}"
CLANGCC="${B1NIX_CLANG:-$(command -v clang)}"
[ -x "$GXX" ] || { echo "cross g++ missing: $GXX"; exit 1; }
[ -n "$CLANGXX" ] || { echo "clang++ not found in PATH"; exit 1; }
RES="$("$CLANGXX" -print-resource-dir)"
SYSROOT="$ROOT_DIR/build/rust-native/rust-src-full/build/x86_64-unknown-linux-gnu/stage2"

B1NIX_OS_DEFINES="-D__b1nix__=1 -D__b1nix=1 -D__unix__=1 -D__unix=1"
B1NIX_CLANG_MATCH="-mcx16 -fno-use-cxa-atexit"
# libc++ build: clang resource headers (builtins) + b1nix libc C headers only.
# NO GCC libstdc++ C++ header dirs — libc++ replaces them (GN adds its -isystem).
# _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE: b1nix is an unknown platform to libc++'s
# locale layer (no glibc/musl/BSD rune table); use libc++'s self-contained
# default ctype table (C-locale ASCII, which is all b1nix supports anyway).
# _LIBCPP_HAS_CLOCK_GETTIME: b1nix has clock_gettime(CLOCK_MONOTONIC), so libc++'s
# steady_clock uses the POSIX clock_gettime path (chrono.cpp); without it libc++
# errors "Monotonic clock not implemented on this platform".
CLANG_CPPFLAGS="--target=x86_64-b1nix $B1NIX_OS_DEFINES $B1NIX_CLANG_MATCH -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -D_LIBCPP_HAS_CLOCK_GETTIME=1 -nostdinc -isystem $RES/include -isystem $CROSS/x86_64-b1nix/include"
FILTER="$ROOT_DIR/tools/v8-clang-filter.sh"

GEN_ARGS="target_os=\"b1nix\" target_cpu=\"x64\" is_clang=false clang_use_chrome_plugins=false \
b1nix_use_clang=true b1nix_clang_cc=\"$FILTER $CLANGCC\" b1nix_clang_cxx=\"$FILTER $CLANGXX\" \
b1nix_clang_cppflags=\"$CLANG_CPPFLAGS\" \
treat_warnings_as_errors=false v8_enable_i18n_support=true icu_use_data_file=false is_debug=false \
v8_jitless=false v8_use_external_startup_data=false symbol_level=0 \
use_custom_libcxx=true \
v8_enable_temporal_support=true v8_enable_sparkplug=true v8_enable_maglev=true v8_enable_turbofan=true \
v8_enable_webassembly=true v8_enable_sandbox=true v8_enable_pointer_compression=true v8_enable_external_code_space=true \
enable_rust=true rust_sysroot_absolute=\"$SYSROOT\" rustc_version=\"b1nix-rust-1.98.0-nightly-01dfd7924\""
if command -v ccache >/dev/null 2>&1; then GEN_ARGS="$GEN_ARGS cc_wrapper=\"ccache\""; fi

cd "$SK/v8"
"$GN" gen out/b1nix-jit-clang-libcxx --args="$GEN_ARGS"
echo "=== gn gen (CLANG + libc++) OK -> out/b1nix-jit-clang-libcxx ==="
