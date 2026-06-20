#!/bin/sh
# Generate a JIT-enabled V8 build graph for b1nix in a SEPARATE out dir
# (out/b1nix-jit), leaving the proven jitless out/b1nix intact as a fallback.
#
# Flips v8_jitless=false and turns on the JIT pipeline. TurboFan is mandatory for
# any non-jitless build (V8 asserts jitless <=> all tiers off), so it goes on with
# Sparkplug (the baseline JIT), Maglev (the mid-tier JIT), and the external code
# space (the separate code cage). All three are verified on b1nix: the full m58.js
# suite (12 markers + done) runs fault-free under Sparkplug AND TurboFan with Maglev
# AND the code cage on. WebAssembly stays OFF (still out of scope).
#
# The code cage was previously off because it appeared to crash; that turned out to
# be a b1nix kernel bug, not V8: sys_mmap honored V8's sub-4 GiB cage hint, which
# collides with the supervisor-only low-4 GiB identity map (2 MB huge pages cloned
# into every address space) — a 4 KiB user PTE can't override it, so the access
# faulted as a present supervisor page. Fixed in kernel/syscall/syscall.c (relocate
# low non-FIXED hints, as vm_find_free_area already does). With that fix the cage
# works, so it is on for the tighter code/data separation.
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
#   v8_enable_maglev=true                  -> mid-tier JIT (verified on b1nix)
#   v8_enable_webassembly=false            -> Wasm still out of scope
#   v8_enable_pointer_compression=true     -> ON: compressed 32-bit data-heap
#                                             pointers (the memory win — important on
#                                             memory-constrained b1nix). Verified
#                                             13/13 on b1nix once the code cage is off.
#   v8_enable_external_code_space=true     -> ON: the separate *code* cage. Verified
#                                             on b1nix once the sys_mmap low-hint bug
#                                             was fixed (see header). The earlier
#                                             "zero base → code ptrs near 0x400 → crash"
#                                             was that mmap bug (V8's low cage hint
#                                             collided with the supervisor identity
#                                             map), not a V8 problem.
"$GN" gen out/b1nix-jit --args='target_os="b1nix" target_cpu="x64" is_clang=false treat_warnings_as_errors=false v8_enable_i18n_support=false is_debug=false v8_jitless=false v8_use_external_startup_data=false symbol_level=0 use_custom_libcxx=false v8_enable_temporal_support=false v8_enable_sparkplug=true v8_enable_maglev=true v8_enable_turbofan=true v8_enable_webassembly=false v8_enable_sandbox=false v8_enable_pointer_compression=true v8_enable_external_code_space=true'

echo
echo "=== gn gen (JIT) OK -> out/b1nix-jit. Next: ==="
echo "  ninja -C $SK/v8/out/b1nix-jit d8     # compile+link chase (TurboFan is large)"
echo "  then relink: sh tools/v8-link-d8.sh  # (point it at out/b1nix-jit)"
