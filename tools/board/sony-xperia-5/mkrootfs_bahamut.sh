#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the full b1nix rootfs for the Xperia 5's internal UFS storage.
#
# The boot image has room for a trimmed shell environment and no more. With
# the UFS driver (kernel/dev/ufs.c, enabled by b1nix.ufs) the whole staged
# rootfs goes on the phone's own flash instead: this image is flashed to
# system_a — slot A's system partition, unused while Android lives on slot B —
# and the kernel mounts it as / because its ext4 label is b1nix-root. That label
# is also what lets the kernel write to it: every other UFS partition is
# read-only to b1nix.
#
#   ROOTFS_MB=1024 mkrootfs_bahamut.sh [ROOTFS_DIR] [OUT]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$(dirname "$(dirname "$DIR")")")"
FULL_ROOTFS=1 RAMDISK_LABEL=b1nix-root RAMDISK_MB="${ROOTFS_MB:-1024}" \
FORCE_RAMDISK="${FORCE_ROOTFS:-1}" \
    sh "$DIR/mkramdisk_bahamut.sh" "${1:-$ROOT/build/aarch64/rootfs}" \
       "${2:-$ROOT/build/aarch64/bahamut-rootfs.ext4}"
