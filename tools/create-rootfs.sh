#!/bin/sh
# create-rootfs.sh — Create a persistent ext4 root filesystem image for B1NIX
# Usage: ./tools/create-rootfs.sh [output_path] [size_mb]
#
# This script complements the Makefile's root-image target, providing
# a standalone way to create a root filesystem image for local development.

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT="${1:-$PROJECT_DIR/build/x86_64/root.ext4}"
SIZE_MB="${2:-32}"

MKE2FS="$(command -v mke2fs 2>/dev/null || command -v /sbin/mke2fs 2>/dev/null || echo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)"

echo "=== B1NIX Root Filesystem Creator ==="
echo "Output: $OUTPUT"
echo "Size:   ${SIZE_MB}MB"
echo ""

# Check prerequisites
if [ ! -x "$MKE2FS" ]; then
    echo "Error: mke2fs not found. Install e2fsprogs."
    exit 1
fi

# Create a staging directory
STAGING="$(mktemp -d)"
mkdir -p "$STAGING/bin" "$STAGING/etc" "$STAGING/dev" \
         "$STAGING/home" "$STAGING/tmp" "$STAGING/var"

# Copy userspace binaries if available
if [ -d "$PROJECT_DIR/userspace/build/bin" ]; then
    cp -r "$PROJECT_DIR/userspace/build/bin/"* "$STAGING/bin/" 2>/dev/null || true
fi

# Install libc and headers
if [ -d "$PROJECT_DIR/userspace/build/libc" ]; then
    mkdir -p "$STAGING/lib"
    cp -r "$PROJECT_DIR/userspace/build/libc/"* "$STAGING/lib/" 2>/dev/null || true
fi

# Create /etc/motd
echo "b1nix persistent root — created $(date)" > "$STAGING/etc/motd"

# Create the ext4 image
dd if=/dev/zero of="$OUTPUT" bs=1048576 count="$SIZE_MB" 2>/dev/null
"$MKE2FS" -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file \
    -q -d "$STAGING" "$OUTPUT" 2>/dev/null || \
    "$MKE2FS" -t ext4 -q -d "$STAGING" "$OUTPUT"

rm -rf "$STAGING"
echo ""
echo "Created $OUTPUT ($(du -sh "$OUTPUT" | cut -f1))"
echo "Mount with: mount -t ext4 $OUTPUT /mnt -o loop"
echo "Boot with:  qemu -drive file=$OUTPUT,format=raw,if=virtio"
