#!/bin/sh
# Put the kernel that was just built straight into a disk image's ESP.
#
#   sh tools/image/push-kernel.sh [IMAGE]
#
# The long way round to test a kernel change is to rebuild the package, publish
# the repository and rebuild the image: three steps and about eight minutes,
# almost all of it spent re-doing work that has nothing to do with the kernel.
# The ESP is a FAT filesystem inside the image and mtools can write to it in
# place, so this does the same job in a couple of seconds.
#
# It deliberately changes ONLY the kernel file. The installed system's dpkg
# database still names the package version it was built with, which is exactly
# right for an iteration loop and exactly wrong for anything shipped: an image
# a person installs is built by mk-b1nix-image.sh from the repository, and
# every release goes that way.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/$ARCH}"
IMG="${1:-$BUILD_DIR/b1nix-disk.img}"
KERNEL_ELF="${KERNEL_ELF:-$BUILD_DIR/kernel.elf}"

log() { printf '\033[1;34m[push-kernel]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[push-kernel] %s\033[0m\n' "$*" >&2; exit 1; }

[ -f "$IMG" ] || die "no image at $IMG"
[ -f "$KERNEL_ELF" ] || die "no kernel at $KERNEL_ELF"
command -v mcopy >/dev/null 2>&1 || die "mtools is not installed"

# The kernel release, composed as the kernel composes it for uname(2).
_vh="$ROOT_DIR/kernel/include/b1nix/version.h"
RELEASE="$(sed -n 's/^#define B1NIX_LINUX_ABI_RELEASE[ \t]*"\([^"]*\)".*/\1/p' "$_vh" | head -1)-b1nix-$(sed -n 's/^#define B1NIX_VERSION_STR[ \t]*"\([^"]*\)".*/\1/p' "$_vh" | head -1)"

# Where the ESP starts, read from the partition table rather than assumed: the
# layout is mk-b1nix-image.sh's business and may change, and a hardcoded offset
# that goes stale would write a kernel into the middle of the root filesystem.
ESP_OFF=$(python3 - "$IMG" <<'PY'
import struct, sys, uuid
ESP = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le
with open(sys.argv[1], "rb") as f:
    f.seek(512)
    hdr = f.read(92)
    if hdr[:8] != b"EFI PART":
        sys.exit("no GPT in image")
    entries_lba, n_entries, esize = struct.unpack_from("<QII", hdr, 72)
    f.seek(entries_lba * 512)
    table = f.read(n_entries * esize)
for i in range(n_entries):
    e = table[i * esize:(i + 1) * esize]
    if e[:16] == ESP:
        print(struct.unpack_from("<Q", e, 32)[0] * 512)
        break
else:
    sys.exit("no EFI system partition in image")
PY
) || die "could not find the ESP"

TMP="$BUILD_DIR/.push-kernel"
rm -rf "$TMP"
mkdir -p "$TMP"
# The same split the package makes: what boots is stripped, and the blob the
# panic path symbolises from has to survive it.
objcopy --strip-all "$KERNEL_ELF" "$TMP/b1nix-$RELEASE"
objdump -h "$TMP/b1nix-$RELEASE" | grep -q '\.kallsyms' ||
	die "the kallsyms section did not survive the strip"

mcopy -o -i "$IMG@@$ESP_OFF" "$TMP/b1nix-$RELEASE" "::/b1nix-$RELEASE" ||
	die "could not write the kernel into the ESP"
rm -rf "$TMP"
log "kernel $RELEASE written into $(basename "$IMG") (ESP at offset $ESP_OFF)"
