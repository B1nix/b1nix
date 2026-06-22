#!/bin/sh
# M64 Phase 2: generate a Clang-frontend V8 build graph in a SEPARATE out dir
# (out/b1nix-jit-clang), leaving the proven GCC build out/b1nix-jit intact as
# the fallback. Same sandbox/JIT/i18n/wasm config as v8-gen-jit.sh — the ONLY
# difference is the compiler frontend: is_clang=true + a host clang compiling
# against the GCC libstdc++ headers (mirrors tools/b1nix-c++). The GNU C++
# runtime is reused (use_custom_libcxx=false); the final d8 is relinked by
# tools/v8-link-d8.sh against the same libstdc++/libsupc++/libgcc group, so only
# the .o compile path differs from the GCC build.
#
#   sh tools/v8-gen-clang.sh
#   ninja -C build/toolchain_build/v8-skeleton/v8/out/b1nix-jit-clang d8
#   sh tools/v8-link-d8.sh b1nix-jit-clang
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SK="$ROOT_DIR/build/toolchain_build/v8-skeleton"
GN="$SK/gn-src/out/gn"
[ -x "$GN" ] || { echo "gn missing — run: sh tools/build-gn.sh"; exit 1; }
[ -d "$SK/v8/src" ] || { echo "v8 checkout missing at $SK/v8 — run sh tools/sync-v8.sh"; exit 1; }

# Re-apply the b1nix GN patches (idempotent) in case the tree was re-synced.
sh "$ROOT_DIR/tools/patches/v8/apply.sh" "$SK/v8" || true

# --- Compute the clang search path (same recipe as tools/b1nix-c++) -----------
CROSS="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/cross"
GXX="$CROSS/bin/x86_64-b1nix-g++"
CLANGXX="${B1NIX_CLANGXX:-$(command -v clang++)}"
CLANGCC="${B1NIX_CLANG:-$(command -v clang)}"
[ -x "$GXX" ] || { echo "cross g++ missing: $GXX"; exit 1; }
[ -n "$CLANGXX" ] || { echo "clang++ not found in PATH"; exit 1; }

VER="$("$GXX" -dumpversion)"
CXXROOT="$CROSS/x86_64-b1nix/include/c++/$VER"
GCCROOT="$CROSS/lib/gcc/x86_64-b1nix/$VER"
RES="$("$CLANGXX" -print-resource-dir)"
# -nostdinc + explicit isystem: clang builtins, then GCC libstdc++ headers, then
# the b1nix libc headers. Applied to both C and C++ via extra_cppflags (the
# C++-only dirs are harmless on C compiles).
# --target=x86_64-b1nix tells clang nothing about the OS, so unlike the b1nix
# cross GCC it predefines none of the b1nix OS identity macros. The tree's OS
# detection (partition_alloc build_config.h, v8config.h, libc headers) keys on
# __b1nix__/__unix__, so define the same set the cross GCC does (see
#   x86_64-b1nix-g++ -dM -E -x c++ /dev/null) — this is what makes clang look
# like the GCC-validated b1nix target.
B1NIX_OS_DEFINES="-D__b1nix__=1 -D__b1nix=1 -D__unix__=1 -D__unix=1"
# Match the b1nix cross GCC's codegen so the clang objects relink against the
# same GNU runtime + libb1nix the GCC build uses:
#   -mcx16             : inline 16-byte CAS (cmpxchg16b) instead of an
#                        __atomic_compare_exchange_16 libcall — the cross GCC
#                        has no libatomic, and its default CPU inlines these.
#   -fno-use-cxa-atexit: register static dtors via atexit() (which libb1nix
#                        provides) instead of __cxa_atexit/__dso_handle, which
#                        nothing on b1nix defines (the cross GCC is built
#                        --disable-__cxa_atexit, so its objects don't need them).
B1NIX_CLANG_MATCH="-mcx16 -fno-use-cxa-atexit"
CLANG_CPPFLAGS="--target=x86_64-b1nix $B1NIX_OS_DEFINES $B1NIX_CLANG_MATCH -nostdinc -isystem $RES/include -isystem $CXXROOT -isystem $CXXROOT/x86_64-b1nix -isystem $CXXROOT/backward -isystem $GCCROOT/include -isystem $GCCROOT/include-fixed -isystem $CROSS/x86_64-b1nix/include"

# --- gn args: v8-gen-jit.sh's GEN_ARGS with is_clang=true ---------------------
# clang_use_chrome_plugins=false: we use a stock host clang, not Chromium's
# bundled build with its plugins. clang_base_path points the version probe at
# our clang. Everything else matches the GCC sandbox build exactly.
# Compile through tools/v8-clang-filter.sh, which strips the bundled-clang-only
# flags //build emits (real compiler passed as its first arg; ccache treats the
# shim as the compiler, which is fine).
#
# NB: global is_clang stays FALSE so the host/snapshot toolchains keep using the
# system GCC (a global is_clang=true wants a bundled compiler-rt the system
# clang lacks). Only b1nix_use_clang=true flips the b1nix target to clang.
# clang_use_chrome_plugins=false drops the -Xclang plugin flags from the target
# clang compiles (we use a stock clang, not Chromium's bundled build).
FILTER="$ROOT_DIR/tools/v8-clang-filter.sh"
GEN_ARGS="target_os=\"b1nix\" target_cpu=\"x64\" is_clang=false clang_use_chrome_plugins=false b1nix_use_clang=true b1nix_clang_cc=\"$FILTER $CLANGCC\" b1nix_clang_cxx=\"$FILTER $CLANGXX\" b1nix_clang_cppflags=\"$CLANG_CPPFLAGS\" treat_warnings_as_errors=false v8_enable_i18n_support=true icu_use_data_file=false is_debug=false v8_jitless=false v8_use_external_startup_data=false symbol_level=0 use_custom_libcxx=false use_safe_libstdcxx=true v8_enable_temporal_support=false v8_enable_sparkplug=true v8_enable_maglev=true v8_enable_turbofan=true v8_enable_webassembly=true v8_enable_sandbox=true v8_enable_pointer_compression=true v8_enable_external_code_space=true"
if command -v ccache >/dev/null 2>&1; then GEN_ARGS="$GEN_ARGS cc_wrapper=\"ccache\""; fi

cd "$SK/v8"
"$GN" gen out/b1nix-jit-clang --args="$GEN_ARGS"

echo
echo "=== gn gen (CLANG) OK -> out/b1nix-jit-clang. Next: ==="
echo "  ninja -C $SK/v8/out/b1nix-jit-clang d8"
echo "  sh tools/v8-link-d8.sh b1nix-jit-clang"
