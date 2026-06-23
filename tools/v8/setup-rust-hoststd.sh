#!/bin/sh
# Graft a HOST (x86_64-unknown-linux-gnu) std into the b1nix cross-rust stage2
# sysroot so V8/Chromium's GN can build Rust PROC MACROS and Cargo BUILD SCRIPTS
# (which run on the host, not on b1nix).
#
# Why this is needed: the native rust bootstrap built stage2 std ONLY for the
# b1nix target (its sysroot has lib/rustlib/x86_64-unknown-b1nix/lib but the
# host lib/rustlib/x86_64-unknown-linux-gnu/lib has no std rlibs). Chromium's
# `clang_x64_for_rust_host_build_tools` toolchain compiles proc-macros/build.rs
# with OUR rustc targeting the host, so it needs a host std in the same sysroot.
#
# We link the rustup nightly host std — it is the SAME compiler commit as our
# stage2 (rustc 1.98.0-nightly 01dfd7924) so it is version-compatible. We must
# link the COMPLETE fileset (.rlib + .rmeta + .so): modern std uses split
# metadata (-Zembed-metadata=no), so the .rlib is a stub and rustc needs the
# matching .rmeta — copying only the .rlib yields "only metadata stub found".
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STAGE2="$ROOT/build/rust-native/rust-src-full/build/x86_64-unknown-linux-gnu/stage2"
DST="$STAGE2/lib/rustlib/x86_64-unknown-linux-gnu/lib"

# Pick the newest rustup nightly toolchain that matches our stage2 commit.
WANT="$("$STAGE2/bin/rustc" -V 2>/dev/null | sed -n 's/.*(\([0-9a-f]*\) .*/\1/p')"
SRC=""
for t in "$ROOT"/build/rust/rustup/toolchains/nightly-*-x86_64-unknown-linux-gnu; do
	[ -d "$t" ] || continue
	have="$("$t/bin/rustc" -V 2>/dev/null | sed -n 's/.*(\([0-9a-f]*\) .*/\1/p')"
	if [ -n "$WANT" ] && [ "$have" = "$WANT" ]; then SRC="$t/lib/rustlib/x86_64-unknown-linux-gnu/lib"; fi
done
[ -n "$SRC" ] || { echo "setup-rust-hoststd: no rustup nightly matching commit $WANT found" >&2; exit 1; }

if ls "$DST"/libstd-*.rlib >/dev/null 2>&1 && ls "$DST"/libstd-*.rmeta >/dev/null 2>&1; then
	echo "setup-rust-hoststd: host std already present in stage2 sysroot"
	exit 0
fi

mkdir -p "$DST"
n=0
for f in "$SRC"/*.rlib "$SRC"/*.rmeta "$SRC"/*.so; do
	[ -f "$f" ] || continue
	ln -sf "$f" "$DST/$(basename "$f")"
	n=$((n + 1))
done
echo "setup-rust-hoststd: linked $n host-std files from $SRC -> $DST"
