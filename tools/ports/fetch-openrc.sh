#!/bin/sh
# Fetch the OpenRC source the port builds from.
#
# There was no fetch step at all: the tree had been cloned by hand once, and
# build/src/openrc kept only the b1nix shim files that live beside it. Cleaning
# the build directory therefore removed the upstream source with no way to get
# it back, and the port failed pointing at a missing file rather than at a
# missing download. The shim files are preserved — they are ours, and they sit
# in the same directory.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SRC_DIR="$ROOT_DIR/build/src/openrc"
VERSION="${OPENRC_VERSION:-0.63.1}"
URL="https://github.com/OpenRC/openrc/archive/refs/tags/${VERSION}.tar.gz"

if [ -f "$SRC_DIR/src/openrc-init/openrc-init.c" ]; then
	echo "fetch-openrc: already present at $SRC_DIR"
	exit 0
fi

echo "fetch-openrc: downloading OpenRC $VERSION" >&2
mkdir -p "$SRC_DIR"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -sL -o "$TMP/openrc.tar.gz" "$URL"
mkdir -p "$TMP/x"
tar -xzf "$TMP/openrc.tar.gz" -C "$TMP/x" --strip-components=1

# -n: never overwrite. The b1nix shim and compat header already in this
# directory are ours and must survive a re-fetch.
cp -rn "$TMP/x"/* "$SRC_DIR"/ 2>/dev/null || true

[ -f "$SRC_DIR/src/openrc-init/openrc-init.c" ] || {
	echo "fetch-openrc: source incomplete after extraction" >&2
	exit 1
}
echo "$SRC_DIR"
