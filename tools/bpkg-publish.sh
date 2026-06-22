#!/bin/sh
# bpkg-publish.sh - host-side publisher for the bpkg package manager.
#
# Takes a built rootfs subtree (a directory whose contents become the package's
# files, extracted relative to / on the target), packages it as a gzipped tar,
# computes its sha256, and appends/replaces the matching line in a local
# pkgs/index. The produced URL points at jsDelivr serving a PUBLIC GitHub repo.
#
# This script does NOT push anything - pushing pkgs/ to a public GitHub repo is
# your manual step (jsDelivr serves from the public repo over its CDN).
#
# Usage:
#   tools/bpkg-publish.sh <srcdir> <name> <version> <arch> [user/repo@ref] [outdir] [deps]
#
# Example:
#   tools/bpkg-publish.sh ./hello-root hello 1.0 x86_64 myuser/b1nix-pkgs@main
#   tools/bpkg-publish.sh ./tcc-root tcc 0.9.27 x86_64 "" "" dev   # depends on dev
#
# Arguments:
#   srcdir       directory tree to package (its contents are tar'd from inside it,
#                so paths are stored relative, e.g. bin/hello)
#   name         package name (index key)
#   version      package version
#   arch         target arch, must match `uname -m` on b1nix: x86_64 or i686
#   user/repo@ref  GitHub <USER>/<REPO>@<REF> for the jsDelivr URL
#                  (default: the GH_SLUG placeholder below)
#   outdir       where pkgs/ lives (default: ./pkgs next to the repo root)
#   deps         OPTIONAL comma-separated dependency package names (no spaces),
#                emitted as the index's 6th field; omit/empty for none.

set -eu

# ── jsDelivr / GitHub slug placeholder ──────────────────────────────────────
# Replace with your own PUBLIC repo, or pass it as arg 5.
GH_SLUG_DEFAULT="B1nix/b1nix-pkgs@main"

usage() {
	sed -n '2,30p' "$0" >&2
	exit 2
}

[ $# -ge 4 ] || usage

SRCDIR="$1"
NAME="$2"
VERSION="$3"
ARCH="$4"
GH_SLUG="${5:-$GH_SLUG_DEFAULT}"
OUTDIR="${6:-$(pwd)/pkgs}"
DEPS="${7:-}"

# Normalize deps: strip spaces, reject anything that would break the flat index
# (no whitespace, only package-name characters and commas).
DEPS="$(printf '%s' "$DEPS" | tr -d '[:space:]')"
case "$DEPS" in
	'') ;;
	*[!A-Za-z0-9._+,-]*) echo "bpkg-publish: deps must be comma-separated package names (got '$DEPS')" >&2; exit 1 ;;
esac

case "$ARCH" in
	x86_64|i686) ;;
	*) echo "bpkg-publish: arch must be x86_64 or i686 (got '$ARCH')" >&2; exit 1 ;;
esac

if [ ! -d "$SRCDIR" ]; then
	echo "bpkg-publish: source dir '$SRCDIR' not found" >&2
	exit 1
fi

# sha256 helper: prefer sha256sum, fall back to `shasum -a 256` (macOS).
sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		echo "bpkg-publish: need sha256sum or shasum" >&2
		exit 1
	fi
}

mkdir -p "$OUTDIR/$ARCH"
INDEX="$OUTDIR/index"
TARBALL_NAME="${NAME}-${VERSION}-${ARCH}.tar.gz"
TARBALL="$OUTDIR/$ARCH/$TARBALL_NAME"

# Deterministic tarball: stored paths are relative to SRCDIR (so they extract
# relative to / on the target). GNU tar flags make the bytes reproducible; plain
# tar still works (just not byte-identical across runs).
if tar --sort=name --mtime='2020-01-01 00:00:00' --owner=0 --group=0 \
       --numeric-owner -czf "$TARBALL" -C "$SRCDIR" . 2>/dev/null; then
	:
else
	tar -czf "$TARBALL" -C "$SRCDIR" .
fi

SHA="$(sha256_of "$TARBALL")"
URL="https://cdn.jsdelivr.net/gh/${GH_SLUG%%@*}@${GH_SLUG##*@}/pkgs/${ARCH}/${TARBALL_NAME}"

# Append/replace the matching (name, arch) line in the index. Lines are kept
# space-separated: name version arch sha256 url. Comments (#) are preserved.
[ -f "$INDEX" ] || {
	echo "# bpkg package index. Fields: name version arch sha256 url" > "$INDEX"
}

TMP="$INDEX.tmp.$$"
# Drop any existing line for this (name, arch), keep everything else.
awk -v n="$NAME" -v a="$ARCH" '
	/^[[:space:]]*#/ { print; next }
	/^[[:space:]]*$/ { print; next }
	{ if ($1 == n && $3 == a) next; print }
' "$INDEX" > "$TMP"
# Emit 5 fields for dependency-free packages (legacy-compatible), 6 fields with
# the optional comma-separated deps appended otherwise.
if [ -n "$DEPS" ]; then
	printf '%s %s %s %s %s %s\n' "$NAME" "$VERSION" "$ARCH" "$SHA" "$URL" "$DEPS" >> "$TMP"
else
	printf '%s %s %s %s %s\n' "$NAME" "$VERSION" "$ARCH" "$SHA" "$URL" >> "$TMP"
fi
mv "$TMP" "$INDEX"

echo "bpkg-publish: wrote $TARBALL"
echo "bpkg-publish: sha256 $SHA"
echo "bpkg-publish: index  $INDEX"
echo "bpkg-publish: url    $URL"
echo ""
echo "NEXT (manual): push '$OUTDIR' to the PUBLIC GitHub repo '${GH_SLUG%%@*}'"
echo "  so jsDelivr can serve it, e.g.:"
echo "    cp -r '$OUTDIR' /path/to/clone-of/${GH_SLUG%%@*}/"
echo "    cd /path/to/clone-of/${GH_SLUG%%@*} && git add pkgs && git commit -m 'add $NAME $VERSION' && git push"
echo "  Then on b1nix set INDEX_URL in /etc/bpkg.conf to:"
echo "    https://cdn.jsdelivr.net/gh/${GH_SLUG%%@*}@${GH_SLUG##*@}/pkgs/index"
