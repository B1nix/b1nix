#!/bin/sh
#
# tools/rpi4/mksd.sh — Prepare an SD card for Raspberry Pi 4 running b1nix (AArch64)
#
# Usage:
#   sh tools/rpi4/mksd.sh [DEST_PATH_OR_VOLUME]
#   sh tools/rpi4/mksd.sh --image [out.img]
#   sh tools/rpi4/mksd.sh --dir [staging_dir]
#
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build/aarch64"
FW_CACHE_DIR="$PROJECT_DIR/tools/blobs/rpi4"

RPI_FW_BASE="https://github.com/raspberrypi/firmware/raw/master/boot"
KERNEL_IMAGE="$BUILD_DIR/Image"
DTB_SRC="$PROJECT_DIR/tools/dts/bcm2711-rpi-4-b.dtb"
DTS_SRC="$PROJECT_DIR/tools/dts/bcm2711-rpi-4-b.dts"

DEST=""
MODE="copy" # "copy" or "image"

show_help() {
    cat <<EOF
b1nix Raspberry Pi 4 SD Card Creator

Usage:
  $0 [PATH]                Copy b1nix boot files to mounted SD card (e.g. /Volumes/BOOT)
  $0 --dir [DIR]           Stage boot files into a local folder
  $0 --image [IMG_FILE]    Create a bootable raw disk image (.img) for dd/BalenaEtcher
  $0 --help                Show this help message

Examples:
  sh tools/rpi4/mksd.sh /Volumes/BOOT
  sh tools/rpi4/mksd.sh --image build/aarch64/b1nix-rpi4-sd.img
EOF
}

# Parse arguments
if [ $# -eq 0 ]; then
    # Try auto-detecting mounted SD card volumes on macOS or Linux
    for v in /Volumes/BOOT /Volumes/boot /Volumes/RPI* /Volumes/SD* /media/$USER/* /mnt/boot; do
        if [ -d "$v" ]; then
            DEST="$v"
            break
        fi
    done
    if [ -z "$DEST" ]; then
        DEST="$BUILD_DIR/sdcard"
        echo "[*] No mounted SD card detected. Staging files to: $DEST"
    fi
else
    case "$1" in
        --help|-h)
            show_help
            exit 0
            ;;
        --dir|-d)
            DEST="$2"
            ;;
        --image|-i)
            MODE="image"
            DEST="${2:-$BUILD_DIR/b1nix-rpi4-sd.img}"
            ;;
        *)
            DEST="$1"
            ;;
    esac
fi

echo "================================================================"
echo " Preparing b1nix Boot SD Card for Raspberry Pi 4 (AArch64)"
echo " Target: $DEST ($MODE mode)"
echo "================================================================"

# 1. Compile DTB if needed
if [ -f "$DTS_SRC" ]; then
    echo "[+] Compiling Device Tree with symbols: $DTS_SRC -> $DTB_SRC..."
    dtc -@ -I dts -O dtb -o "$DTB_SRC" "$DTS_SRC" 2>/dev/null || dtc -I dts -O dtb -o "$DTB_SRC" "$DTS_SRC"
fi

# 2. Check or build b1nix AArch64 kernel (KERNEL_BASE=0x80000 for Raspberry Pi 4 RAM at 0)
echo "[+] Building b1nix AArch64 kernel for Raspberry Pi 4 (KERNEL_BASE=0x80000)..."
make -C "$PROJECT_DIR" ARCH=aarch64 KERNEL_BASE=0x80000

if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "[-] Error: Failed to find or build $KERNEL_IMAGE"
    exit 1
fi

# 3. Cache / download Raspberry Pi 4 firmware files (start4.elf, fixup4.dat, overlays/disable-bt.dtbo)
mkdir -p "$FW_CACHE_DIR/overlays"
for fw in start4.elf fixup4.dat overlays/disable-bt.dtbo; do
    if [ ! -f "$FW_CACHE_DIR/$fw" ]; then
        echo "[+] Downloading $fw from upstream Raspberry Pi repository..."
        curl -sSL "$RPI_FW_BASE/$fw" -o "$FW_CACHE_DIR/$fw" || {
            echo "[-] Error downloading $fw. Check internet connection."
            exit 1
        }
    fi
done

# 4. Stage boot files
STAGE_DIR="$BUILD_DIR/.sd-staging"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/overlays"

echo "[+] Populating staging directory..."

# Firmware & Overlays
cp -f "$FW_CACHE_DIR/start4.elf" "$STAGE_DIR/start4.elf"
cp -f "$FW_CACHE_DIR/fixup4.dat" "$STAGE_DIR/fixup4.dat"
cp -f "$FW_CACHE_DIR/overlays/disable-bt.dtbo" "$STAGE_DIR/overlays/disable-bt.dtbo"

# Kernel & DTB
cp -f "$KERNEL_IMAGE" "$STAGE_DIR/kernel8.img"
if [ -f "$DTB_SRC" ]; then
    cp -f "$DTB_SRC" "$STAGE_DIR/bcm2711-rpi-4-b.dtb"
fi

# config.txt
cat <<'EOF' > "$STAGE_DIR/config.txt"
# b1nix Raspberry Pi 4 Boot Configuration
arm_64bit=1
kernel=kernel8.img
enable_uart=1
uart_2ndstage=1
core_freq=500
core_freq_min=500
dtoverlay=disable-bt
EOF




# cmdline.txt
cat <<'EOF' > "$STAGE_DIR/cmdline.txt"
console=ttyAMA0,115200 earlycon=pl011,0xfe201000 root=/dev/ram0 rdinit=/bin/init
EOF

if [ "$MODE" = "copy" ]; then
    mkdir -p "$DEST"
    echo "[+] Copying files to $DEST..."
    cp -Rf "$STAGE_DIR/"* "$DEST/"
    sync
    echo ""
    echo "================================================================"
    echo " [✓] SD Card preparation complete!"
    echo " Files written to: $DEST"
    echo "================================================================"
    ls -lh "$DEST"
elif [ "$MODE" = "image" ]; then
    IMG_SIZE_MB=64
    echo "[+] Creating $IMG_SIZE_MB MB FAT32 disk image at: $DEST..."
    mkdir -p "$(dirname "$DEST")"
    dd if=/dev/zero of="$DEST" bs=1m count="$IMG_SIZE_MB" status=none
    
    if command -v mformat >/dev/null 2>&1 && command -v mcopy >/dev/null 2>&1; then
        mformat -i "$DEST" -F -v B1NIX_BOOT ::
        for f in "$STAGE_DIR"/*; do
            mcopy -i "$DEST" "$f" ::"$(basename "$f")"
        done
        echo "[✓] Image populated using mtools."
    else
        echo "[!] Note: mtools not found. Image created as empty raw file. Copy files manually or format with diskutil."
    fi
    echo "[✓] Disk image created: $DEST"
fi

rm -rf "$STAGE_DIR"

echo ""
echo "--- Next Steps to Boot Raspberry Pi 4 ---"
echo "1. Insert the FAT32 SD card into your Raspberry Pi 4."
echo "2. Connect your RP2350 (Pico 2) UART Bridge:"
echo "   - GP0 (Pico Pin 1) -> RPi 4 Pin 10 (RXD / GPIO 15)"
echo "   - GP1 (Pico Pin 2) -> RPi 4 Pin 8  (TXD / GPIO 14)"
echo "   - GND (Pico Pin 3) -> RPi 4 Pin 6  (GND)"
echo "3. Connect Pico 2 to your PC via USB."
echo "4. Open serial terminal:  screen /dev/cu.usbmodem* 115200"
echo "   (Or run automated monitor:  sh tools/rpi4/hil.sh uart)"
echo "5. Power on the Raspberry Pi 4!"
echo "================================================================"
