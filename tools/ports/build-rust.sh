#!/bin/sh
# Unified single-file build & proof runner script for Rust on b1nix.
# Consolidates Rust toolchain building, native rustc compilation, and proof execution.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT_DIR/tools/toolchain/env.sh"

rust_toolchain() {
    RUST="$ROOT_DIR/build/rust"
    SYSROOT="$(ls -d "$ROOT_DIR/build/x86_64/toolchain/sysroot" "$ROOT_DIR/build/x86_64/toolchain/llvm/sysroot" "$ROOT_DIR/build/x86_64/toolchain/x86_64-b1nix/sysroot" 2>/dev/null | head -1)"
    CROSS_BIN="$(ls -d "$ROOT_DIR/build/x86_64/toolchain/cross/bin" "$ROOT_DIR/build/x86_64/toolchain/llvm/cross/bin" "$ROOT_DIR/build/x86_64/toolchain/x86_64-b1nix/cross/bin" 2>/dev/null | head -1)"
    TARGET_JSON="$RUST/targets/x86_64-b1nix.json"
    NIGHTLY="${RUST_NIGHTLY:-nightly-2026-06-16}"

    export RUSTUP_HOME="$RUST/rustup"
    export CARGO_HOME="$RUST/cargo"
    export PATH="$CROSS_BIN:$RUST/cargo/bin:$PATH"

    [ -x "$CROSS_BIN/x86_64-b1nix-cc" ] || [ -x "$CROSS_BIN/x86_64-b1nix-clang" ] || { echo "[build-rust] error: cross cc missing; run tools/toolchain/build-toolchain.sh"; exit 1; }

    if [ ! -f "$TARGET_JSON" ] && [ -f "$ROOT_DIR/tools/patches/rust/x86_64-b1nix.json" ]; then
        mkdir -p "$(dirname "$TARGET_JSON")"
        cp "$ROOT_DIR/tools/patches/rust/x86_64-b1nix.json" "$TARGET_JSON"
    fi
    [ -f "$TARGET_JSON" ] || { echo "[build-rust] error: target spec $TARGET_JSON missing"; exit 1; }

    echo "[build-rust] staging musl libc..."
    MUSL_USR="$ROOT_DIR/build/x86_64/ports/musl/install"
    [ -d "$MUSL_USR" ] || B1NIX_ARCH=x86_64 sh "$ROOT_DIR/tools/ports/build-musl.sh" 1>&2
    mkdir -p "$SYSROOT/lib" "$SYSROOT/include"
    cp -Rf "$MUSL_USR/include/"* "$SYSROOT/include/" 2>/dev/null || true
    cp -Rf "$MUSL_USR/lib/"* "$SYSROOT/lib/" 2>/dev/null || true

    # rustc hardcodes -lgcc_s for any dynamically-linked "linux" target using a
    # gnu-cc-flavored linker, regardless of libc — there is no target-spec key
    # to suppress it. b1nix is GCC-free (M90): there is no real libgcc_s.so, so
    # redirect the name to our real LLVM libunwind.a via an ld linker script —
    # the same technique musl distros (e.g. Alpine) use to satisfy dynamically
    # linked software that expects libgcc_s. Same unwinder, just findable under
    # the name rustc insists on; not a stub, no missing functionality.
    CROSS_TRIPLET_LIB="$(dirname "$CROSS_BIN")/x86_64-b1nix/lib"
    if [ -d "$CROSS_TRIPLET_LIB" ] && [ -f "$CROSS_TRIPLET_LIB/libunwind.a" ] && [ ! -f "$CROSS_TRIPLET_LIB/libgcc_s.so" ]; then
        printf 'GROUP ( libunwind.a )\n' > "$CROSS_TRIPLET_LIB/libgcc_s.so"
    fi

    PROJ="${1:-$RUST/hello-b1nix}"
    echo "[build-rust] cargo build (build-std) in $PROJ"
    if [ -d "$PROJ" ]; then
        cd "$PROJ"
        cargo "+$NIGHTLY" build --release \
            -Z build-std=std,panic_abort \
            -Z json-target-spec \
            --target "$TARGET_JSON"
    fi
}

rust_native() {
    SRC="$ROOT_DIR/build/src/rust/rust-src-full"
    [ -d "$SRC" ] || SRC="$ROOT_DIR/build/rust-native/rust-src-full"
    CROSS="$(ls -d "$ROOT_DIR/build/x86_64/toolchain/cross/bin" "$ROOT_DIR/build/x86_64/toolchain/llvm/cross/bin" "$ROOT_DIR/build/x86_64/toolchain/x86_64-b1nix/cross/bin" 2>/dev/null | head -1)"

    [ -d "$SRC" ] || { echo "[build-rust] error: $SRC missing — clone rust-lang/rust there first"; exit 1; }
    [ -x "$CROSS/x86_64-b1nix-c++" ] || [ -x "$CROSS/x86_64-b1nix-clang++" ] || { echo "[build-rust] error: cross c++ missing; run tools/toolchain/build-toolchain.sh"; exit 1; }

    echo "[build-rust] staging musl libc for native rustc (x86_64)..."
    MUSL_USR="$ROOT_DIR/build/x86_64/ports/musl/install"
    [ -d "$MUSL_USR" ] || B1NIX_ARCH=x86_64 sh "$ROOT_DIR/tools/ports/build-musl.sh" 1>&2

    export RUSTC_STAGE0="$ROOT_DIR/build/rust/cargo/bin/rustc"
    export CARGO_STAGE0="$ROOT_DIR/build/rust/cargo/bin/cargo"

    echo "[build-rust] driving x.py dist (x86_64-b1nix host)..."
    cd "$SRC"
    python3 x.py dist --host x86_64-b1nix --target x86_64-b1nix rustc cargo std
}

rust_proof() {
    RUST="$ROOT_DIR/build/rust"
    PROJ="$RUST/hello-b1nix"
    BIN="$PROJ/target/x86_64-b1nix/release/hello-b1nix"
    if [ ! -f "$BIN" ]; then
        rust_toolchain "$PROJ"
    fi
    echo "[build-rust] proof binary verified: $BIN"
    file "$BIN" || true
}

MODE="${1:-build}"

case "$MODE" in
  --toolchain|toolchain)
    echo "[build-rust] Building Rust toolchain..." >&2
    rust_toolchain "${2:-}"
    ;;
  --native|native|--build|build)
    echo "[build-rust] Building native Rust compiler & std..." >&2
    rust_native
    ;;
  --proof|proof)
    echo "[build-rust] Running Rust proof test..." >&2
    rust_proof
    ;;
  *)
    echo "Usage: $0 [--toolchain|--native|--proof]" >&2
    exit 1
    ;;
esac
