#!/bin/sh
# clean-llvm-build.sh — nuke the b1nix cross-LLVM build directory so
# x.py rebuilds it from scratch with fresh cmake configuration.
#
# Usage: sh tools/clean-llvm-build.sh
#   Then run: sh tools/build-rust-native.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_BUILD="$ROOT/build/rust-native/rust-src-full/build/x86_64-unknown-b1nix/llvm/build"
if [ -d "$LLVM_BUILD" ]; then
    rm -rf "$LLVM_BUILD"
    echo ">> removed $LLVM_BUILD"
else
    echo ">> $LLVM_BUILD already absent — nothing to do"
fi
