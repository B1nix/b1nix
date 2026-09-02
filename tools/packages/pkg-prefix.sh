#!/bin/sh
# Install a port that is now an Alpine package, and print where it landed.
#
# This replaces the ten tools/ports/build-<name>.sh scripts that used to build
# these libraries from source: they all did the same thing after the migration —
# fetch, and echo the prefix — so they are one script and one table
# (tools/packages/alpine-ports.map) instead of ten near-identical files.
#
# The contract is the one the port scripts had, because their callers depend on
# it: the last line of stdout is the install prefix, and it contains include/
# and lib/ laid out the way the from-source build laid them out.
#
# Usage:  tools/packages/pkg-prefix.sh <port>
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-${ARCH:-x86_64}}"
MAP="$ROOT_DIR/tools/packages/alpine-ports.map"

# --into <dir> installs somewhere other than the build prefix, which is what
# putting a program on the image means: paired with ALPINE_LAYOUT=native it
# unpacks the package where it expects to live.
PREFIX=""
EXPLICIT_PREFIX=0
if [ "${1:-}" = "--into" ]; then
	PREFIX="$2"; shift 2
	EXPLICIT_PREFIX=1
fi
[ $# -eq 1 ] || { echo "usage: $0 [--into <dir>] <port>" >&2; exit 2; }
PORT="$1"
[ -n "$PREFIX" ] || PREFIX="$ROOT_DIR/build/$ARCH/pkg/$PORT"

# Explicit installs target the shared rootfs, so the directory itself cannot
# tell us whether this particular port has already been unpacked.  Keep a
# small stamp per port and include the map checksum: changing the package map
# invalidates the stamp without forcing a full clean of the rootfs.
MAP_SIG="$(cksum "$MAP" | awk '{print $1 ":" $2}')"
STAMP="$PREFIX/.b1nix-pkg-$PORT.stamp"

if [ "$EXPLICIT_PREFIX" -eq 0 ] && [ -d "$PREFIX" ] && [ -d "$PREFIX/lib" -o -d "$PREFIX/include" ]; then
	echo "$PREFIX"
	exit 0
fi

if [ "$EXPLICIT_PREFIX" -eq 1 ] && [ -f "$STAMP" ] &&
	[ "$(sed -n '1p' "$STAMP")" = "$MAP_SIG" ]; then
	echo "$PREFIX"
	exit 0
fi

# Continuation lines: a long package list is wrapped with a trailing backslash.
PKGS="$(sed -e :a -e '/\\$/N; s/\\\n//; ta' "$MAP" |
        awk -v p="$PORT" '$1 == p { $1 = ""; print; exit }')"
[ -n "$PKGS" ] || {
	echo "pkg-prefix: $PORT is not in $(basename "$MAP")" >&2
	exit 1
}

# A repo: token picks which Alpine repository to look in. Most of what is
# wanted here is in main; libvpx and libjxl are in community, and the index is
# per-repository, so the choice belongs with the package list rather than with
# the caller.
case "$PKGS" in
*repo:*)
	ALPINE_REPO="$(printf '%s\n' $PKGS | sed -n 's/^repo://p')"
	PKGS="$(printf '%s\n' $PKGS | grep -v '^repo:' | tr '\n' ' ')"
	export ALPINE_REPO
	;;
esac

# Package list is word-split intentionally
ARCH="$ARCH" "$ROOT_DIR/tools/packages/alpine-fetch.sh" "$PREFIX" $PKGS >&2

# Dated now, not when the package was built.
#
# tar restores the archive's timestamps, and an Alpine package built in 2023
# unpacks a library older than every prerequisite make compares it against. The
# rule is then out of date the moment it finishes, on every build, forever —
# which is how twenty-three package rules came to re-extract themselves each
# time and, through the stamp they feed, drag a five-minute Skia/Dawn rebuild
# along with them.
find "$PREFIX" -exec touch {} + 2>/dev/null || true

if [ "$EXPLICIT_PREFIX" -eq 1 ]; then
	printf '%s\n' "$MAP_SIG" > "$STAMP"
fi

echo "$PREFIX"
