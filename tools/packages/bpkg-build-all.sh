#!/bin/sh
# bpkg-build-all.sh — package every app we ship from a built b1nix rootfs into
# pkgs/ (for the b1nix-pkgs repo) via tools/packages/bpkg-publish.sh.
#
# Idempotent: an entry is packaged only if ALL its files exist in the rootfs;
# missing entries are skipped with a note. So it can be re-run as more ports get
# built (e.g. first against a shell-suite rootfs, then again after the
# graphics/net ports are built) and only adds what is newly available.
#
# Usage: tools/packages/bpkg-build-all.sh <rootfs> <arch> <outdir-pkgs> [user/repo@ref]
#   rootfs       a built b1nix rootfs (e.g. build/x86_64/rootfs)
#   arch         x86_64  (must match `uname -m` on the target)
#   outdir-pkgs  the pkgs/ dir to write into (e.g. ~/b1nix-pkgs/pkgs)
set -eu

ROOTFS="$1"; ARCH="$2"; OUT="$3"; SLUG="${4:-B1nix/b1nix-pkgs@main}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PUBLISH="$HERE/bpkg-publish.sh"
[ -d "$ROOTFS" ] || { echo "rootfs '$ROOTFS' not found" >&2; exit 1; }
[ -x "$PUBLISH" ] || { echo "bpkg-publish.sh not found at $PUBLISH" >&2; exit 1; }

# name | version | space-separated paths relative to the rootfs | optional deps
# The 4th field (comma-separated dependency package names) is OPTIONAL.
#
# 'kernel' is the base system package: the compile-capable sysroot (static
# libs + crt0 + the full headers tree). With it installed, an on-target b1cc
# can actually build and link programs, so b1cc depends on it and
# `bpkg install b1cc` pulls it in transitively. Everything beyond this pair is
# meant to come from Alpine rather than be published here.
MANIFEST='
kernel|1.0|lib/libc.a lib/libm.a lib/libb1nix.a lib/libb1gui.a lib/libunwind.a? lib/crt0.o lib/b1cc? include|
b1cc|1.0|bin/b1cc|kernel
'

printf '%s\n' "$MANIFEST" | while IFS='|' read -r name version paths deps; do
	[ -n "$name" ] || continue
	# A path is optional if it ends with '?': package it when present, skip it
	# silently when absent (used for arch-specific extras like libunwind.a).
	missing=; reqpaths=
	for p in $paths; do
		case "$p" in
			*'?') p="${p%\?}"; [ -e "$ROOTFS/$p" ] && reqpaths="$reqpaths $p" ;;
			*)    if [ -e "$ROOTFS/$p" ]; then reqpaths="$reqpaths $p"; else missing="$missing $p"; fi ;;
		esac
	done
	if [ -n "$missing" ]; then
		echo "SKIP  $name (missing:$missing)"
		continue
	fi
	stage="$(mktemp -d)"
	# `cp --parents` is a GNU extension BSD/macOS cp does not have. Recreate the
	# directory prefix by hand and copy each path into it, dereferencing symlinks
	# (-L) exactly as before, so this stages identically on either host.
	( cd "$ROOTFS" && for _p in $reqpaths; do
		mkdir -p "$stage/$(dirname "$_p")"
		cp -RL "$_p" "$stage/$_p"
	done )
	echo "PACK  $name $version  [$reqpaths]${deps:+  deps=$deps}"
	"$PUBLISH" "$stage" "$name" "$version" "$ARCH" "$SLUG" "$OUT" "$deps" \
		| grep -E 'sha256|url' | sed 's/^/      /'
	rm -rf "$stage"
done

echo ""
echo "index: $OUT/index"
