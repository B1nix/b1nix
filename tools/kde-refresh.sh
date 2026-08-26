#!/bin/sh
# kde-refresh.sh — re-stage only the rootfs-overlay files and rebuild the image.
#
# A full `make iso` re-copies the whole KDE package root file by file, which is
# a quarter of an hour for a one-line change to a shell script that is already
# in the tree. This copies the overlay, rebuilds the filesystem and the ISO, and
# nothing else. Use `make` for anything that compiles.
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
CMDLINE=${1:-b1nix.kde}
OUT=${2:-b1nix.iso}
cd "$DIR"
# The WHOLE overlay, not just /etc.
#
# This copied etc/ alone, which silently skipped everything else the overlay
# carries -- /root/.profile among it, the file that launches the desktop inside
# a login session. A refresh that leaves out part of the change is worse than
# no refresh: the image builds, boots, and behaves as though the edit was never
# made. userspace/Makefile stages the same directory the same way.
cp -R userspace/rootfs-overlay/. build/x86_64/rootfs/
dd if=/dev/zero of=build/x86_64/root.ext4 bs=1048576 count=2560 2>/dev/null
mke2fs -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q -L b1nix-root \
	-E root_owner=0:0 -d build/x86_64/rootfs build/x86_64/root.ext4
sh tools/mkiso.sh --stage build/x86_64/iso --out "build/x86_64/$OUT" \
	--arch x86_64 --kernel build/x86_64/kernel.elf --timeout 0 \
	--cmdline "$CMDLINE" --module build/x86_64/root.ext4:rootfs.img
echo "kde-refresh: build/x86_64/$OUT cmdline=[$CMDLINE]"
