#!/bin/sh
# Unified single-file build & runner script for V8 on b1nix.
# Consolidates sync, GN generation, build, linking, and QEMU execution.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT_DIR/tools/toolchain/env.sh"

SK="$ROOT_DIR/build/x86_64/toolchain/v8-skeleton"
GN="$SK/gn-src/out/gn"

build_gn() {
    mkdir -p "$SK"
    cd "$SK"
    if [ -d gn-src ] && ! git -C gn-src describe HEAD --match initial-commit >/dev/null 2>&1; then
        echo "[build-v8] removing shallow gn-src and re-cloning full..."
        rm -rf gn-src
    fi
    [ -d gn-src ] || git clone https://gn.googlesource.com/gn gn-src
    cd gn-src
    python3 build/gen.py
    ninja -C out gn
    echo "[build-v8] gn built: $(pwd)/out/gn"
}

v8_sync() {
    [ -x "$GN" ] || build_gn
    export DEPOT_TOOLS_UPDATE=0 DEPOT_TOOLS_METRICS=0
    if [ ! -d "$SK/depot_tools" ]; then
        git clone --depth 1 \
            https://chromium.googlesource.com/chromium/tools/depot_tools.git \
            "$SK/depot_tools"
    fi
    export PATH="$SK/depot_tools:$PATH"

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

    [ -L "$SK/v8/build" ] && rm -f "$SK/v8/build"
    cd "$SK"
    gclient sync --no-history -D
    sh "$ROOT_DIR/tools/patches/v8/apply.sh" "$SK/v8"

    cd "$SK/v8"
    "$GN" gen out/b1nix --args='target_os="b1nix" target_cpu="x64" is_clang=true treat_warnings_as_errors=false v8_enable_i18n_support=false is_debug=false v8_jitless=true v8_use_external_startup_data=false symbol_level=0 use_custom_libcxx=false v8_enable_temporal_support=false v8_enable_sparkplug=false v8_enable_maglev=false v8_enable_turbofan=false v8_enable_webassembly=false v8_enable_sandbox=false'
    echo "[build-v8] gn gen completed successfully."
}

v8_build() {
    [ -x "$GN" ] || build_gn
    [ -d "$SK/v8/out/b1nix" ] || v8_sync
    echo "[build-v8] Building V8 (d8 & v8_libbase) under $SK/v8/out/b1nix..."
    ninja -C "$SK/v8/out/b1nix" v8_libbase d8 || true
}

v8_run() {
    ISO="${ISO:-$ROOT_DIR/build/x86_64/b1nix.iso}"
    LOG="${LOG:-$ROOT_DIR/smoke_run/v8-run.log}"
    TIMEOUT="${TIMEOUT:-120}"

    [ -f "$ISO" ] || { echo "[build-v8] missing $ISO — build the x86_64 ISO first"; exit 1; }
    mkdir -p "$ROOT_DIR/smoke_run"

    accel=""
    if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
        accel="-accel kvm"
    fi

    set -- qemu-system-x86_64 $accel -m "${SMOKE_MEM_MB:-2048}" \
        -cdrom "$ISO" -serial stdio -display none -monitor none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04

    echo "[build-v8] $*"
    "$@" >"$LOG" 2>&1 &
    pid=$!

    start=$(date +%s)
    while :; do
        if grep -qaE "M58-V8: done|KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
            break
        fi
        kill -0 "$pid" 2>/dev/null || break
        [ $(( $(date +%s) - start )) -ge "$TIMEOUT" ] && { echo "[v8-run] timeout ${TIMEOUT}s"; break; }
        sleep 1
    done
    kill "$pid" 2>/dev/null || true
    sleep 1
    kill -9 "$pid" 2>/dev/null || true

    echo "==================== v8 run result ===================="
    grep -aoE "M58-V8: (ok [a-z-]+|done|hello)" "$LOG" | sort -u | sed 's/^/  /' || true
}

MODE="${1:-build}"
case "$MODE" in
  --sync|sync)
    v8_sync
    ;;
  --build|build)
    v8_build
    ;;
  --run|run)
    v8_run
    ;;
  *)
    echo "Usage: $0 [--sync|--build|--run]" >&2
    exit 1
    ;;
esac
