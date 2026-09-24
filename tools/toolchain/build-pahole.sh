#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# tools/toolchain/build-pahole.sh - Build pahole, which turns the kernel's
# DWARF into BTF (/sys/kernel/btf/vmlinux).
#
# BTF is what a CO-RE program is relocated against: libbpf reads the running
# kernel's type information and rewrites each field access to the offset this
# kernel actually has. The tool that produces it is pahole, the one Linux's own
# build uses; writing a second DWARF-to-BTF converter would be a second copy of
# a format every BPF loader reads back.
#
# Pinned by tag and commit, and built from source into build/ so no host
# package is needed. Prints the path of the pahole binary.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${PAHOLE_VERSION:-1.31}"
COMMIT="1f2805b6eef104df3125143c949b391f6122e5b9"
URL="https://git.kernel.org/pub/scm/devel/pahole/pahole.git"
SRC="$ROOT_DIR/build/src/pahole-$VERSION"
OUT="$ROOT_DIR/build/tools/pahole-$VERSION"

if [ -x "$OUT/bin/pahole" ]; then
	echo "$OUT/bin/pahole"
	exit 0
fi

if [ ! -d "$SRC/.git" ]; then
	mkdir -p "$(dirname "$SRC")"
	git clone -q --depth 1 --branch "v$VERSION" --recurse-submodules \
		--shallow-submodules "$URL" "$SRC" >&2
fi
have="$(git -C "$SRC" rev-parse HEAD)"
if [ "$have" != "$COMMIT" ]; then
	echo "build-pahole: v$VERSION is $have, expected $COMMIT" >&2
	exit 1
fi

cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$OUT" -D__LIB=lib >&2
cmake --build "$SRC/build" -j6 >&2
cmake --install "$SRC/build" >&2
echo "$OUT/bin/pahole"
