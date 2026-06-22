#!/bin/sh
# bpkg-build-all.sh — package every app we ship from a built b1nix rootfs into
# pkgs/ (for the b1nix-pkgs repo) via tools/bpkg-publish.sh.
#
# Idempotent: an entry is packaged only if ALL its files exist in the rootfs;
# missing entries are skipped with a note. So it can be re-run as more ports get
# built (e.g. first against a shell-suite rootfs, then again after the
# graphics/net ports are built) and only adds what is newly available.
#
# Usage: tools/bpkg-build-all.sh <rootfs> <arch> <outdir-pkgs> [user/repo@ref]
#   rootfs       a built b1nix rootfs (e.g. build/x86_64/rootfs)
#   arch         x86_64 | i686  (must match `uname -m` on the target)
#   outdir-pkgs  the pkgs/ dir to write into (e.g. ~/b1nix-pkgs/pkgs)
set -eu

ROOTFS="$1"; ARCH="$2"; OUT="$3"; SLUG="${4:-B1nix/b1nix-pkgs@main}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PUBLISH="$HERE/bpkg-publish.sh"
[ -d "$ROOTFS" ] || { echo "rootfs '$ROOTFS' not found" >&2; exit 1; }
[ -x "$PUBLISH" ] || { echo "bpkg-publish.sh not found at $PUBLISH" >&2; exit 1; }

# name | version | space-separated paths relative to the rootfs.
# tcc bundles its runtime (crt0/libc/libm + headers + tcc's own includes) so a
# `bpkg install tcc` yields a compiler that can actually build a program.
MANIFEST='
bash|5.2|bin/bash
tcc|0.9.27|bin/tcc lib/tcc lib/crt0.o lib/libc.a lib/libb1nix.a lib/libm.a include
js|1.0|bin/js
hello|1.0|bin/hello
gpaint|1.0|bin/gpaint
gclock|1.0|bin/gclock
gterm|1.0|bin/gterm
gdesktop|1.0|bin/gdesktop
gabout|1.0|bin/gabout
curl|8.11|bin/curl
wget|1.25|bin/wget
dropbear|2024|bin/dropbear bin/dbclient bin/dropbearkey
make|4.4|bin/make
openssl|3.4|bin/openssl
netsurf|3.11|bin/netsurf-fb
'

printf '%s\n' "$MANIFEST" | while IFS='|' read -r name version paths; do
	[ -n "$name" ] || continue
	missing=
	for p in $paths; do [ -e "$ROOTFS/$p" ] || missing="$missing $p"; done
	if [ -n "$missing" ]; then
		echo "SKIP  $name (missing:$missing)"
		continue
	fi
	stage="$(mktemp -d)"
	( cd "$ROOTFS" && cp -a --parents $paths "$stage/" )
	echo "PACK  $name $version  [$paths]"
	"$PUBLISH" "$stage" "$name" "$version" "$ARCH" "$SLUG" "$OUT" \
		| grep -E 'sha256|url' | sed 's/^/      /'
	rm -rf "$stage"
done

echo ""
echo "index: $OUT/index"
