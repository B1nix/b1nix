#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Flash the b1nix rootfs to system_a and the kernel boot image to boot_a, then
# boot slot A. Built by `make bahamut-ufs`.
#
# system_a is slot A's system partition. On this phone (retrofit dynamic
# partitions) it is the "super" of slot A — Android on slot B does not use it,
# but an Android OTA would overwrite it, and slot A will no longer boot Android.
# restore_slot_b.sh switches back to Android as before.
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
B1NIX_ROOT="$(dirname "$(dirname "$(dirname "$DIR")")")"
BOOT="${1:-$B1NIX_ROOT/build/aarch64/b1nix_bahamut_boot.img}"
ROOTFS="${2:-$B1NIX_ROOT/build/aarch64/bahamut-rootfs.ext4}"

for f in "$BOOT" "$ROOTFS"; do
    [ -f "$f" ] || { echo "[!] $f not found — run 'make bahamut-ufs'"; exit 1; }
done
if [ -z "$(fastboot devices 2>/dev/null)" ]; then
    echo "[!] No device in fastboot mode."
    echo "    Hold Volume Up while plugging the cable in — the LED turns blue."
    exit 1
fi

# Refuse an image the partition cannot hold rather than letting fastboot
# write a truncated filesystem.
want=$(wc -c < "$ROOTFS" | tr -d ' ')
have=$(fastboot getvar partition-size:system_a 2>&1 | awk '/partition-size:system_a:/ {print $2}')
if [ -n "$have" ]; then
    have=$((have))
    echo "[*] system_a: $((have / 1048576)) MiB, rootfs image: $((want / 1048576)) MiB"
    [ "$want" -le "$have" ] || { echo "[!] rootfs image is larger than system_a"; exit 1; }
else
    echo "[?] bootloader did not report system_a's size; flashing anyway"
fi

if [ "${YES:-0}" != "1" ]; then
    read -r -p "Overwrite system_a and boot_a on this phone? [y/N] " a
    [ "$a" = "y" ] || [ "$a" = "Y" ] || exit 1
fi

fastboot flash system_a "$ROOTFS"
fastboot flash boot_a "$BOOT"
fastboot --set-active=a
fastboot reboot
echo "[+] Booting b1nix with its rootfs on UFS (system_a)."
