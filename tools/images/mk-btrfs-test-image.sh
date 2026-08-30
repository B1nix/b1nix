#!/bin/sh
# A minimal real btrfs filesystem, for the tests that need one.
#
# Built rather than committed: a binary blob in the tree is a thing nobody can
# review, and this one is reproducible from three lines of mkfs.
#
# 16 MiB is the smallest mkfs.btrfs will produce, and only with mixed block
# groups and 4 KiB nodes — smaller sizes are refused outright. Skips quietly
# when btrfs-progs is absent; the tests that use the image say so themselves
# rather than reporting a pass they did not earn.
set -eu

BUILD_DIR="${1:?usage: mk-btrfs-test-image.sh <build-dir>}"
IMG="$BUILD_DIR/btrfs-test.img"
SEED="$BUILD_DIR/btrfs-seed"

command -v mkfs.btrfs >/dev/null 2>&1 || exit 0
[ -f "$IMG" ] && exit 0

rm -rf "$SEED"
mkdir -p "$SEED"
printf 'b1nix btrfs in-use image\n' > "$SEED/in-use.txt"

rm -f "$IMG"
truncate -s 16M "$IMG"
mkfs.btrfs -q -M -n 4096 -L B1NIX-INUSE --rootdir "$SEED" "$IMG"
rm -rf "$SEED"
