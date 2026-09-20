#!/bin/sh
# Fetch liburing and build its test suite, for the Debian lane to run (M125).
#
# liburing's tests are the only honest answer to "is this io_uring ABI the one
# programs are compiled against". Debian ships the library but not the suite, so
# it is built here, on the host, and copied onto the Debian image.
#
# The binaries are STATIC-PIE on purpose. Building them dynamically would mean
# putting gcc, binutils and libc6-dev on the image and compiling 217 programs
# inside QEMU; building them against the host's glibc would produce binaries
# Debian's older glibc cannot load. Static sidesteps both, and what is under
# test is the kernel's system-call behaviour, which a static binary exercises
# exactly as a dynamic one does. (The tree's dynamic-linking rule in
# tools/check/check-dynamic.sh is about b1nix's OWN test binaries; it does not
# scan this directory, and nothing here is built with b1nix-musl-cc.)
#
# PIE and not plain -static: a fixed-address static ET_EXEC faults in its own
# image on this kernel before main() is reached — every one of the 217 died
# with a write to a present read-only page around 0x4xxxxx — which is a
# separate defect in the ELF loader's handling of a static executable's
# segment permissions, and nothing to do with io_uring. -static-pie loads at
# the ordinary userspace base and runs.
#
# Pinned by version and SHA256, like tools/fs/fetch-linux-fs.sh. Nothing under
# the staged tree is edited.
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
ARCH="${ARCH:-x86_64}"

LIBURING_VERSION="${LIBURING_VERSION:-2.12}"
case "$LIBURING_VERSION" in
2.12) LIBURING_SHA256="f1d10cb058c97c953b4c0c446b11e9177e8c8b32a5a88b309f23fdd389e26370" ;;
*) echo "fetch-liburing: no pinned SHA256 for liburing-$LIBURING_VERSION" >&2; exit 1 ;;
esac

TARBALL="liburing-${LIBURING_VERSION}.tar.gz"
URL="https://github.com/axboe/liburing/archive/refs/tags/liburing-${LIBURING_VERSION}.tar.gz"
CACHE="$BUILD_DIR/src/liburing"
SRC="$CACHE/liburing-liburing-${LIBURING_VERSION}"
OUT="${OUT:-$BUILD_DIR/$ARCH/liburing}"

mkdir -p "$CACHE"

if [ ! -f "$CACHE/$TARBALL" ]; then
	echo "fetch-liburing: downloading $TARBALL" >&2
	curl -L "$URL" -o "$CACHE/$TARBALL.part" 1>&2
	mv "$CACHE/$TARBALL.part" "$CACHE/$TARBALL"
fi

have="$(sha256sum "$CACHE/$TARBALL" | cut -d' ' -f1)"
if [ "$have" != "$LIBURING_SHA256" ]; then
	echo "fetch-liburing: SHA256 mismatch for $TARBALL" >&2
	echo "  expected $LIBURING_SHA256" >&2
	echo "  got      $have" >&2
	exit 1
fi

if [ ! -d "$SRC" ]; then
	echo "fetch-liburing: unpacking $TARBALL" >&2
	tar -xzf "$CACHE/$TARBALL" -C "$CACHE"
fi

# A finished build is left alone: this costs a couple of minutes and nothing
# about it changes between runs.
if [ -f "$OUT/.stamp-$LIBURING_VERSION" ]; then
	echo "fetch-liburing: $OUT is current" >&2
	exit 0
fi

cd "$SRC"
[ -f config-host.mak ] || ./configure >/dev/null
make -j"${JOBS:-6}" -C src >/dev/null
make -j"${JOBS:-6}" -C test LDFLAGS="-static-pie" \
	CFLAGS="-g -O2 -Wall -D_GNU_SOURCE -fPIE" >/dev/null

rm -rf "$OUT"
mkdir -p "$OUT"
for t in test/*.t; do
	cp "$t" "$OUT/"
done
strip -s "$OUT"/*.t 2>/dev/null || true
touch "$OUT/.stamp-$LIBURING_VERSION"
echo "fetch-liburing: $(ls "$OUT"/*.t | wc -l) tests in $OUT" >&2
