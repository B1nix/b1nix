#!/bin/sh
# Stage the big native toolchain packages (llvm, clang, rust) for b1nix-pkgs.
#
# These are too large for jsDelivr (libLLVM.so ~184MB), so they ship via GitHub
# Releases: this script strips the binaries, builds the tarballs into the
# b1nix-pkgs repo's pkgs/<arch>/, and writes index lines whose URL points at a
# Release asset. It does NOT push anything — review the staged tarballs + index,
# then (manually) create the Release and upload:
#   gh release create $TAG pkgs/x86_64/{llvm,clang,rust}-*-x86_64.tar.gz -R B1nix/b1nix-pkgs
#
# Usage: tools/packages/stage-toolchains.sh [pkgs-repo-dir] [release-tag]
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PKGS="${1:-$HOME/Documents/GitHub/b1nix-pkgs/pkgs}"
TAG="${2:-toolchains-v0.69}"
ARCH=x86_64
SLUG_REPO="B1nix/b1nix-pkgs"
REL_BASE="https://github.com/$SLUG_REPO/releases/download/$TAG"

RB="$ROOT/build/rust-native/rust-src-full/build/x86_64-unknown-b1nix"
LLVM_LIB="$RB/llvm/lib"
CLANGD="$ROOT/build/native-clang/b1nix"
RUSTD="$RB/stage2"
STRIP="$(command -v llvm-strip || command -v strip)"

[ -d "$PKGS" ] || { echo "pkgs repo dir not found: $PKGS" >&2; exit 1; }
mkdir -p "$PKGS/$ARCH"
STAGED_INDEX="$PKGS/index.toolchains-staged"
: > "$STAGED_INDEX"

sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1 || shasum -a 256 "$1" | cut -d' ' -f1; }

# pack <name> <version> <stagedir> <deps>
pack() {
	name="$1"; ver="$2"; sdir="$3"; deps="$4"
	tb="$name-$ver-$ARCH.tar.gz"
	tar --sort=name --mtime='2020-01-01 00:00:00' --owner=0 --group=0 \
		-czf "$PKGS/$ARCH/$tb" -C "$sdir" . 2>/dev/null || tar -czf "$PKGS/$ARCH/$tb" -C "$sdir" .
	s="$(sha "$PKGS/$ARCH/$tb")"
	sz="$(du -h "$PKGS/$ARCH/$tb" | cut -f1)"
	printf '%s %s %s %s %s/%s %s\n' "$name" "$ver" "$ARCH" "$s" "$REL_BASE" "$tb" "$deps" >> "$STAGED_INDEX"
	echo "  staged $tb ($sz)  deps=$deps"
	rm -rf "$sdir"
}

echo "[stage-toolchains] -> $PKGS/$ARCH  (tag $TAG)"

# ── llvm: the shared libLLVM.so (stripped) — the backend both clang and rust use
S="$(mktemp -d)"; mkdir -p "$S/lib"
real="$(ls "$LLVM_LIB"/libLLVM.so.* | head -1)"
cp -a "$real" "$S/lib/"; "$STRIP" --strip-unneeded "$S/lib/$(basename "$real")" 2>/dev/null || true
# preserve the SONAME symlink clang/rust DT_NEEDED resolves
( cd "$S/lib" && for l in "$LLVM_LIB"/libLLVM-*.so; do [ -L "$l" ] && ln -sf "$(basename "$real")" "$(basename "$l")"; done )
pack llvm 22.1 "$S" ""

# ── clang: driver + clang++ + libclang-cpp.so + builtin resource headers
S="$(mktemp -d)"; mkdir -p "$S/bin" "$S/lib"
cp -a "$CLANGD/bin/clang-22" "$S/bin/"; "$STRIP" --strip-unneeded "$S/bin/clang-22" 2>/dev/null || true
( cd "$S/bin" && ln -sf clang-22 clang && ln -sf clang clang++ )
creal="$(ls "$CLANGD/lib"/libclang-cpp.so.* | head -1)"
cp -a "$creal" "$S/lib/"; "$STRIP" --strip-unneeded "$S/lib/$(basename "$creal")" 2>/dev/null || true
( cd "$S/lib" && [ -L "$CLANGD/lib/libclang-cpp.so" ] && ln -sf "$(basename "$creal")" libclang-cpp.so )
cp -a "$CLANGD/lib/clang" "$S/lib/"   # resource headers (lib/clang/22/include)
pack clang 22.1 "$S" "llvm,dev"

# ── rust: rustc + librustc_driver.so + std sysroot
S="$(mktemp -d)"; mkdir -p "$S/bin" "$S/lib"
cp -a "$RUSTD/bin/rustc" "$S/bin/"; "$STRIP" --strip-unneeded "$S/bin/rustc" 2>/dev/null || true
for so in "$RUSTD/lib"/librustc_driver-*.so "$RUSTD/lib"/lib*-*.so; do
	[ -f "$so" ] || continue
	cp -a "$so" "$S/lib/"; "$STRIP" --strip-unneeded "$S/lib/$(basename "$so")" 2>/dev/null || true
done
cp -a "$RUSTD/lib/rustlib" "$S/lib/"   # std sysroot
pack rust 1.98 "$S" "llvm,dev"

echo "[stage-toolchains] done. staged index lines:"
cat "$STAGED_INDEX"
echo
echo "NOT pushed. To publish: gh release create $TAG \"$PKGS/$ARCH\"/{llvm,clang,rust}-*-$ARCH.tar.gz -R $SLUG_REPO"
echo "then merge $STAGED_INDEX into $PKGS/index and push the repo."
