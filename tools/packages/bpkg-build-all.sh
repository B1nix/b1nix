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
#   arch         x86_64 | i686  (must match `uname -m` on the target)
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
# 'dev' is the compile-capable sysroot: static libs + crt0 + the full headers
# tree. With it installed, an on-target b1cc/make can actually build and link
# programs. b1cc and make therefore depend on 'dev', so `bpkg install b1cc`
# pulls the sysroot transitively.
MANIFEST='
dev|1.0|lib/libc.a lib/libm.a lib/libb1nix.a lib/libb1gui.a lib/libunwind.a? lib/crt0.o lib/b1cc? include|
zsh|5.9|bin/zsh|
b1cc|1.0|bin/b1cc|dev
js|1.0|bin/js|
hello|1.0|bin/hello|
gpaint|1.0|bin/gpaint|
gclock|1.0|bin/gclock|
gterm|1.0|bin/gterm|
gdesktop|1.0|bin/gdesktop|
gabout|1.0|bin/gabout|
curl|8.20.0|bin/curl|
wget|1.21.4|bin/wget|
dropbear|2022.83|bin/dropbearmulti bin/dropbear bin/dbclient bin/dropbearkey|
make|3.82|bin/make|dev
openssl|1.1.1w|bin/openssl|
netsurf|3.11|bin/netsurf-fb|
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
	( cd "$ROOTFS" && cp -aL --parents $reqpaths "$stage/" )
	echo "PACK  $name $version  [$reqpaths]${deps:+  deps=$deps}"
	"$PUBLISH" "$stage" "$name" "$version" "$ARCH" "$SLUG" "$OUT" "$deps" \
		| grep -E 'sha256|url' | sed 's/^/      /'
	rm -rf "$stage"
done

echo ""
echo "index: $OUT/index"
