#!/bin/sh
# Unified build & sync entrypoint for Chromium on b1nix.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT_DIR/tools/toolchain/env.sh"

CR="${CHROMIUM_DIR:-$ROOT_DIR/build/src/chromium}"
SK="$ROOT_DIR/build/$B1NIX_ARCH/toolchain/v8-skeleton"
GN="$SK/gn-src/out/gn"

chromium_sync() {
    [ -x "$GN" ] || { echo "[build-chromium] gn missing — run: sh tools/v8/build-v8.sh --sync"; exit 1; }

    MIN_GB="${MIN_GB:-90}"
    avail_gb=$(df -P "$(dirname "$CR")" 2>/dev/null | awk 'NR==2{print int($4/1024/1024)}')
    if [ "${avail_gb:-0}" -lt "$MIN_GB" ]; then
        echo "[build-chromium] ABORT: only ${avail_gb:-?} GB free at $CR; need >=${MIN_GB} GB." >&2
        exit 1
    fi

    mkdir -p "$CR"
    cd "$CR"

    if [ ! -d depot_tools ]; then
        git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
    fi
    export PATH="$CR/depot_tools:$PATH"

    if [ ! -d src ]; then
        fetch --no-history chromium
    fi
    cd src
    gclient sync --no-history

    sh "$ROOT_DIR/tools/patches/chromium/apply.sh" "$CR/src"

    mkdir -p out/b1nix
    CC_WRAPPER=""
    if command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ]; then
        CC_WRAPPER='cc_wrapper = "ccache"'
    fi

    cat > out/b1nix/args.gn <<EOF
target_os = "b1nix"
target_cpu = "x64"
is_clang = true
$CC_WRAPPER
is_debug = false
is_component_build = false
use_sysroot = false
use_ozone = true
ozone_platform_headless = true
ozone_auto_platforms = false
enable_supervised_users = true
use_custom_libcxx = false
v8_enable_sandbox = false
EOF
    "$GN" gen out/b1nix
    echo "[build-chromium] gn gen completed for Chromium."
}

chromium_build() {
    [ -d "$CR/src/out/b1nix" ] || chromium_sync
    echo "[build-chromium] Building Chromium base target..."
    ninja -C "$CR/src/out/b1nix" base || true
}

MODE="${1:-build}"
case "$MODE" in
  --sync|sync)
    chromium_sync
    ;;
  --build|build)
    chromium_build
    ;;
  *)
    echo "Usage: $0 [--sync|--build]" >&2
    exit 1
    ;;
esac
