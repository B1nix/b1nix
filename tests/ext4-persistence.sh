#!/bin/sh
# Format SATA and NVMe images as ext4 (minimal features for kernel compat)
# The kernel ext4 driver does not support metadata_csum or 64bit features.
set -e

SATA_IMG="$1"
NVME_IMG="$2"

MKE2FS="/opt/homebrew/opt/e2fsprogs/sbin/mke2fs"
if [ ! -x "$MKE2FS" ]; then
    MKE2FS=$(command -v mke2fs 2>/dev/null || echo "/sbin/mke2fs")
fi

# Format with minimal ext4 features
"$MKE2FS" -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$SATA_IMG" 2>/dev/null
"$MKE2FS" -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$NVME_IMG" 2>/dev/null
echo "ext4 images formatted successfully"
