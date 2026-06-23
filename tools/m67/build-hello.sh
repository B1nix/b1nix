#!/bin/sh
# build-hello.sh — regenerate tools/m67/hello_b1nix.elf (M67 Rust port smoke).
#
# Compiles tools/m67/hello_b1nix.rs for x86_64-unknown-b1nix with nightly rustc
# + -Zbuild-std and copies the resulting static b1nix ELF here as a committed
# blob (so the kernel build does NOT require the Rust toolchain — same pattern
# as tools/m40/linux_hello.bin). Run this by hand after changing the source or
# the Rust target spec / libc shims, then commit the refreshed .elf.
#
# Prereqs: build/rust/{rustup,cargo} nightly + rust-src, the cross gcc, and
# build/rust/targets/x86_64-unknown-b1nix.json — all set up by the M67 port.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tools/m67"
PROJ="$ROOT/build/rust/hello-b1nix"

# Keep the canonical source in sync into the cargo project, then build.
cp "$HERE/hello_b1nix.rs" "$PROJ/src/main.rs"
sh "$ROOT/tools/build-rust-toolchain.sh" "$PROJ"
cp "$PROJ/target/x86_64-unknown-b1nix/release/hello-b1nix" "$HERE/hello_b1nix.elf"
echo ">> refreshed $HERE/hello_b1nix.elf"
