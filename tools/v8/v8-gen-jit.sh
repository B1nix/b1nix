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
#   sh tools/v8/v8-gen-jit.sh
#   ninja -C build/toolchain_build/v8-skeleton/v8/out/b1nix-jit d8
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SK="$ROOT_DIR/build/toolchain_build/v8-skeleton"
GN="$SK/gn-src/out/gn"
[ -x "$GN" ] || { echo "gn missing — run: sh tools/v8/build-gn.sh"; exit 1; }
[ -d "$SK/v8/src" ] || { echo "v8 checkout missing at $SK/v8 — run sh tools/v8/sync-v8.sh"; exit 1; }

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
#   v8_enable_sandbox=true                 -> ON: the V8 sandbox (TrustedSpace +
#                                             sandboxed pointers). Verified on
#                                             b1nix once two things were fixed:
#                                             (1) build — Patch 21/22 in
#                                             tools/patches/v8/apply.sh (Linux
#                                             crash-filter off, stack_trace_linux
#                                             compiled for CollectStackTrace);
#                                             (2) runtime — a b1nix sys_mmap bug
#                                             where a pure PROT_NONE reservation
#                                             was eagerly marked page-by-page, so
#                                             the sandbox's ~1.4 TiB cage + 256
#                                             back-to-back 4 GiB Smi-range
#                                             reservations ran hundreds of
#                                             millions of iterations and hung the
#                                             boot. Fixed by skipping eager
#                                             marking for PROT_NONE (lazy
#                                             fault-in). Needs use_safe_libstdcxx
#                                             (the libstdc++ hardening the sandbox
#                                             assert demands; Patch 18).
#   v8_enable_i18n_support=true            -> ON: ICU (Intl). Data EMBEDDED via
#                                             icu_use_data_file=false (no external
#                                             icudtl.dat). Needs LC_MESSAGES in the
#                                             libc <locale.h> (ICU putil.cpp).
#                                             Verified: M58-V8: ok intl.
#   v8_enable_webassembly=true             -> ON. Builds with the trap handler off
#                                             (Patch 20 -> explicit bounds checks).
#                                             The startup abort ("AllowHeapAllocation
#                                             InRelease" during snapshot deserialize)
#                                             was NOT a V8 bug: b1nix placed the
#                                             main-thread TLS pointer at
#                                             region+round_up(memsz,align) while the
#                                             b1nix linker emits local-exec offsets
#                                             as symbol_offset-memsz (un-rounded), so
#                                             a TLS memsz not divisible by align (wasm
#                                             d8: 0x108) mis-read every __thread by the
#                                             padding. Fixed in process.c + pthread.c
#                                             (TP = region+memsz). Verified: ok wasm.
GEN_ARGS='target_os="b1nix" target_cpu="x64" is_clang=false treat_warnings_as_errors=false v8_enable_i18n_support=true icu_use_data_file=false is_debug=false v8_jitless=false v8_use_external_startup_data=false symbol_level=0 use_custom_libcxx=false use_safe_libstdcxx=true v8_enable_temporal_support=false v8_enable_sparkplug=true v8_enable_maglev=true v8_enable_turbofan=true v8_enable_webassembly=true v8_enable_sandbox=true v8_enable_pointer_compression=true v8_enable_external_code_space=true'
# Route the compiler through ccache when present — flipping global gn flags
# recompiles ~everything, but ccache makes re-runs/flip-backs near-instant.
if command -v ccache >/dev/null 2>&1; then GEN_ARGS="$GEN_ARGS cc_wrapper=\"ccache\""; fi
"$GN" gen out/b1nix-jit --args="$GEN_ARGS"

echo
echo "=== gn gen (JIT) OK -> out/b1nix-jit. Next: ==="
echo "  ninja -C $SK/v8/out/b1nix-jit d8     # compile+link chase (TurboFan is large)"
echo "  then relink: sh tools/v8/v8-link-d8.sh  # (point it at out/b1nix-jit)"
