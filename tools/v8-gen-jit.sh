#!/bin/sh
# Generate a JIT-enabled V8 build graph for b1nix in a SEPARATE out dir
# (out/b1nix-jit), leaving the proven jitless out/b1nix intact as a fallback.
#
# Flips v8_jitless=false and turns on the JIT pipeline. TurboFan is mandatory for
# any non-jitless build (V8 asserts jitless <=> all tiers off), so it goes on with
# Sparkplug (the baseline JIT). Maglev (mid-tier) and WebAssembly stay OFF to keep
# the first JIT port's surface area down — enable them once d8 --jit runs.
#
# Prereqs (same as sync-v8.sh, already cached): gn, the synced+patched v8 checkout.
# No gclient sync here — the tree is already synced and apply.sh'd.
#
#   sh tools/v8-gen-jit.sh
#   ninja -C build/toolchain_build/v8-skeleton/v8/out/b1nix-jit d8
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SK="$ROOT_DIR/build/toolchain_build/v8-skeleton"
GN="$SK/gn-src/out/gn"
[ -x "$GN" ] || { echo "gn missing — run: sh tools/build-gn.sh"; exit 1; }
[ -d "$SK/v8/src" ] || { echo "v8 checkout missing at $SK/v8 — run sh tools/sync-v8.sh"; exit 1; }

# Re-apply the b1nix GN patches (idempotent) in case the tree was re-synced.
sh "$ROOT_DIR/tools/patches/v8/apply.sh" "$SK/v8" || true

cd "$SK/v8"
# Same base args as the jitless gen (sync-v8.sh) EXCEPT:
#   v8_jitless=false                       -> enable the JIT
#   v8_enable_sparkplug=true               -> baseline JIT tier
#   v8_enable_turbofan=true                -> optimizing tier (mandatory when not jitless)
#   v8_enable_maglev=false                 -> skip the mid-tier for the first port
#   v8_enable_webassembly=false            -> Wasm still out of scope
#   v8_enable_pointer_compression=false    -> JIT'd code decompresses pointers with
#                                             a cage base that comes out wrong on
#                                             b1nix (every decompressed pointer lands
#                                             near-null → crash). Full 64-bit pointers
#                                             sidestep the cage entirely for the first
#                                             JIT bring-up; revisit the cage base later.
#   v8_enable_external_code_space=false    -> depends on pointer compression
"$GN" gen out/b1nix-jit --args='target_os="b1nix" target_cpu="x64" is_clang=false treat_warnings_as_errors=false v8_enable_i18n_support=false is_debug=false v8_jitless=false v8_use_external_startup_data=false symbol_level=0 use_custom_libcxx=false v8_enable_temporal_support=false v8_enable_sparkplug=true v8_enable_maglev=false v8_enable_turbofan=true v8_enable_webassembly=false v8_enable_sandbox=false v8_enable_pointer_compression=false v8_enable_external_code_space=false'

echo
echo "=== gn gen (JIT) OK -> out/b1nix-jit. Next: ==="
echo "  ninja -C $SK/v8/out/b1nix-jit d8     # compile+link chase (TurboFan is large)"
echo "  then relink: sh tools/v8-link-d8.sh  # (point it at out/b1nix-jit)"
