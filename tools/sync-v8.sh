#!/bin/sh
# Phase 2 of the V8 -> b1nix port: depot_tools + a full `gclient sync` of the V8
# tree, then (re-)apply the b1nix patches and `gn gen` the target.
#
# RUN THIS YOURSELF — it downloads multi-GB of third-party source (the DEPS
# sub-trees + sysroot/toolchain hooks), which Claude isn't allowed to do
# unattended (same reason as tools/build-gn.sh).
#
#   sh tools/sync-v8.sh
#
# Prereqs already cached on this box (all survive `make clean`, which spares
# build/toolchain_build/):
#   - cross g++:  build/toolchain_build/x86_64-b1nix/cross/bin/x86_64-b1nix-g++
#   - gn:         build/toolchain_build/v8-skeleton/gn-src/out/gn   (tools/build-gn.sh)
#   - v8 proper:  build/toolchain_build/v8-skeleton/v8              (shallow clone)
#
# Result: build/toolchain_build/v8-skeleton/v8/out/b1nix with a generated ninja
# graph. The next step (the "chase __linux__ sites" loop) is:
#   ninja -C build/toolchain_build/v8-skeleton/v8/out/b1nix v8_libbase
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SK="$ROOT_DIR/build/toolchain_build/v8-skeleton"
GN="$SK/gn-src/out/gn"
[ -x "$GN" ] || { echo "gn missing — run: sh tools/build-gn.sh"; exit 1; }
[ -d "$SK/v8/src" ] || { echo "v8 checkout missing at $SK/v8"; exit 1; }

# 1. depot_tools (provides gclient). Pin auto-update off so it can't churn the
#    pinned tool revisions mid-sync.
export DEPOT_TOOLS_UPDATE=0 DEPOT_TOOLS_METRICS=0
if [ ! -d "$SK/depot_tools" ]; then
  git clone --depth 1 \
    https://chromium.googlesource.com/chromium/tools/depot_tools.git \
    "$SK/depot_tools"
fi
export PATH="$SK/depot_tools:$PATH"

# 2. .gclient — managed=False so gclient leaves our existing (shallow) v8 git
#    checkout alone and only syncs the DEPS sub-trees (//build, buildtools,
#    third_party/*) at the revisions pinned in v8/DEPS.
cat > "$SK/.gclient" <<'EOF'
solutions = [
  {
    "name": "v8",
    "url": "https://chromium.googlesource.com/v8/v8.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": { "checkout_clang_tidy": False },
  },
]
EOF

# The skeleton symlinked v8/build -> ../build (a separately-cloned //build). The
# real DEPS-pinned //build must replace it, so drop the symlink before sync.
[ -L "$SK/v8/build" ] && rm -f "$SK/v8/build"

# 3. The big download. --no-history keeps sub-deps shallow; -D prunes deps that
#    were removed from DEPS. Hooks run (they generate //build/util/LASTCHANGE etc.
#    that `gn gen` imports). If a hook chokes on a vendored-toolchain download,
#    re-run with `--nohooks` appended and create build/util/LASTCHANGE by hand.
cd "$SK"
gclient sync --no-history -D

# 4. (re-)apply the b1nix GN patches. gclient just re-pulled //build at its
#    pinned revision, wiping Patches 1 & 2 — apply.sh restores them (idempotent).
sh "$ROOT_DIR/tools/patches/v8/apply.sh" "$SK/v8"

# 5. generate the b1nix target build graph (jitless, no i18n, no snapshot data).
cd "$SK/v8"
# Arg notes (validated):
#   is_clang=false                       -> GCC build; else toolchain rules inject
#                                           clang-only -Xclang/module/plugin flags
#   treat_warnings_as_errors=false       -> GCC warns differently than clang; V8's
#                                           -Werror would otherwise fail the build
#   v8_jitless + all JIT/Wasm tiers off  -> required by the jitless assert
#   v8_enable_temporal_support=false     -> Temporal is Rust; we build no Rust
#   v8_enable_sandbox=false              -> sandbox needs libc++ hardening; we use libstdc++
#   use_custom_libcxx=false              -> GCC/libstdc++, not vendored libc++
"$GN" gen out/b1nix --args='target_os="b1nix" target_cpu="x64" is_clang=false treat_warnings_as_errors=false v8_enable_i18n_support=false is_debug=false v8_jitless=true v8_use_external_startup_data=false symbol_level=0 use_custom_libcxx=false v8_enable_temporal_support=false v8_enable_sparkplug=false v8_enable_maglev=false v8_enable_turbofan=false v8_enable_webassembly=false v8_enable_sandbox=false'

echo
echo "=== gn gen OK. Next (the __linux__-site chase): ==="
echo "  ninja -C $SK/v8/out/b1nix v8_libbase"
