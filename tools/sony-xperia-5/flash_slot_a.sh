#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
B1NIX_ROOT="$(dirname "$(dirname "$DIR")")"
IMAGE="${1:-$B1NIX_ROOT/build/aarch64/b1nix_bahamut_boot.img}"

if [ ! -f "$IMAGE" ]; then
    echo "[!] Image $IMAGE not found! Running pack script first..."
    python3 "$DIR/mkbootimg_bahamut.py"
fi

echo "[*] Checking Fastboot device..."
fastboot devices
# `fastboot flash` BLOCKS waiting for a device rather than failing, and
# `fastboot devices` exits 0 whether or not it found one — so a phone that is
# hung, booted, or unplugged turns a flash into a silent wait with no output.
# Check for a real device and say what to do instead.
if [ -z "$(fastboot devices 2>/dev/null)" ]; then
    echo "[!] No device in fastboot mode."
    echo "    Force a reboot with Power + Volume Up (hold ~10 s), then hold"
    echo "    Volume Up while plugging the cable in — the LED turns blue."
    exit 1
fi

echo "[*] Flashing $IMAGE to boot_a..."
fastboot flash boot_a "$IMAGE"

echo "[*] Setting active slot to A..."
fastboot --set-active=a

echo "[*] Rebooting into B1NIX on Xperia 5..."
fastboot reboot

echo "[+] Done! B1NIX is now running on your Xperia 5 display."
