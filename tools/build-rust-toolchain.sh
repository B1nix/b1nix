#!/bin/sh
# build-rust-toolchain.sh — Rust → b1nix cross build (M67).
#
# Builds a Rust program for the x86_64-unknown-b1nix target using nightly rustc
# + -Zbuild-std. b1nix is linux-ABI-aliased: the target spec sets os=linux /
# env=musl and reuses Rust std's unix PAL unchanged, linking against b1nix's
# POSIX libc (libb1nix.a == libc.a) via the x86_64-b1nix-gcc cross linker.
#
# Prerequisites (already present in build/, all gitignored):
#   - build/rust/{rustup,cargo}        nightly toolchain + rust-src component
#   - build/rust/targets/x86_64-unknown-b1nix.json   the rustc target spec
#   - build/toolchain_build/x86_64-b1nix/cross/bin/x86_64-b1nix-gcc  cross linker
#
# This script also (re)stages the cross-gcc sysroot from the freshly built
# userspace libc, so a libc change is reflected in the Rust link.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUST="$ROOT/build/rust"
SYSROOT="$ROOT/build/toolchain_build/x86_64-b1nix/sysroot"
CROSS_BIN="$ROOT/build/toolchain_build/x86_64-b1nix/cross/bin"
TARGET_JSON="$RUST/targets/x86_64-unknown-b1nix.json"
NIGHTLY="${RUST_NIGHTLY:-nightly-2026-06-16}"

export RUSTUP_HOME="$RUST/rustup"
export CARGO_HOME="$RUST/cargo"
export PATH="$CROSS_BIN:$RUST/cargo/bin:$PATH"

[ -x "$CROSS_BIN/x86_64-b1nix-gcc" ] || { echo "error: cross gcc missing; run tools/build-toolchain.sh"; exit 1; }
# The JSON target spec lives (gitignored) in build/rust/targets; stage the tracked
# canonical copy from tools/patches/rust on a fresh checkout so M67 is reproducible.
if [ ! -f "$TARGET_JSON" ] && [ -f "$ROOT/tools/patches/rust/x86_64-unknown-b1nix.json" ]; then
    mkdir -p "$(dirname "$TARGET_JSON")"
    cp "$ROOT/tools/patches/rust/x86_64-unknown-b1nix.json" "$TARGET_JSON"
fi
[ -f "$TARGET_JSON" ]               || { echo "error: target spec $TARGET_JSON missing"; exit 1; }

# ── 1. Build the b1nix userspace libc + crt0 and stage them into the sysroot ──
echo ">> building b1nix userspace libc + crt0"
make -C "$ROOT/userspace" -s B1NIX_ARCH=x86_64 \
    build/x86_64/libb1nix.a build/x86_64/crt/crt0.o

echo ">> staging sysroot ($SYSROOT)"
mkdir -p "$SYSROOT/lib" "$SYSROOT/include"
cp "$ROOT/userspace/build/x86_64/crt/crt0.o"  "$SYSROOT/lib/crt0.o"
cp "$ROOT/userspace/build/x86_64/libb1nix.a"  "$SYSROOT/lib/libb1nix.a"
cp "$ROOT/userspace/build/x86_64/libb1nix.a"  "$SYSROOT/lib/libc.a"
cp -r "$ROOT/userspace/include/." "$SYSROOT/include/"

# M90 GCC-free: the target spec links the LLVM runtimes for the unwinder and
# compiler builtins instead of libgcc_s/libgcc_eh. The `llvm-libunwind: "system"`
# target option makes Rust's `unwind` crate emit `-lunwind` (LLVM libunwind)
# rather than `-lgcc_s`, and the spec's late-link-args add `-lunwind`/`-lcompiler_rt`.
# Stage both archives into the sysroot the cross linker searches.
_TC="$ROOT/build/toolchain_build/x86_64-b1nix"
for _rt in libunwind.a libcompiler_rt.a; do
    _src="$_TC/sysroot/usr/lib/$_rt"
    [ -f "$_src" ] || _src="$_TC/llvm-runtimes-build/install/lib/$_rt"
    [ -f "$_src" ] || _src="$_TC/cross/x86_64-b1nix/lib/$_rt"
    if [ -f "$_src" ]; then
        if ! [ "$_src" -ef "$SYSROOT/lib/$_rt" ]; then
            cp -f "$_src" "$SYSROOT/lib/$_rt"
        fi
    else
        echo "error: LLVM runtime $_rt not found (run tools/toolchain/build-toolchain.sh)" >&2
        exit 1
    fi
done

# ── 2. Build the Rust program (-Zbuild-std builds core/alloc/std from source) ─
PROJ="${1:-$RUST/hello-b1nix}"
echo ">> cargo build (build-std) in $PROJ"
cd "$PROJ"
cargo "+$NIGHTLY" build --release \
    -Z build-std=std,panic_abort \
    -Z json-target-spec \
    --target "$TARGET_JSON"

BIN="$PROJ/target/x86_64-unknown-b1nix/release/$(basename "$PROJ")"
echo ">> built: $BIN"
file "$BIN"
"$CROSS_BIN/x86_64-b1nix-readelf" -h "$BIN" | grep -E "Type:|Machine:|Entry"
