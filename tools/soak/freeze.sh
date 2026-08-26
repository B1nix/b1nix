#!/bin/sh
# Snapshot the current build as a set of soak images the loop can run from.
#
# One ISO per workload, because the workload is on the kernel command line and
# the command line is baked into the image. Freezing them means the overnight
# loop boots one known kernel all night while the tree it came from goes on
# being edited — otherwise a run halfway through a rebuild describes a system
# that never existed.
set -eu
SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
ROOT_DIR="$(cd "$(dirname "$SELF")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"
BUILD="$ROOT_DIR/build/$ARCH"
DEST="${1:-$ROOT_DIR/smoke_run/soak/frozen}"
SECS="${SOAK_SECONDS:-15}"
SCALE="${SOAK_SCALE:-100}"
WORKLOADS="${SOAK_WORKLOADS:-mem vm cpu fd shm spawn disk net gfx all}"

rm -rf "$DEST"
mkdir -p "$DEST"

echo "freezing into $DEST (secs=$SECS scale=$SCALE)"
make -C "$ROOT_DIR" --no-print-directory -j6 iso-soak SOAK_SPEC=mem \
	SOAK_EXTRA="b1nix.soak-seconds=$SECS b1nix.soak-scale=$SCALE" >/dev/null
cp "$BUILD/root.ext4" "$DEST/root.ext4"

for w in $WORKLOADS; do
	make -C "$ROOT_DIR" --no-print-directory iso-soak-quick SOAK_SPEC="$w" \
		SOAK_EXTRA="b1nix.soak-seconds=$SECS b1nix.soak-scale=$SCALE" >/dev/null
	cp "$BUILD/b1nix-soak.iso" "$DEST/b1nix-soak-$w.iso"
	echo "  $w"
done
cp "$BUILD/kernel.elf" "$DEST/kernel.elf"
grep -m1 B1NIX_VERSION_STR "$ROOT_DIR/kernel/include/b1nix/version.h" > "$DEST/VERSION"
echo "frozen: $(ls "$DEST" | wc -l) files"
