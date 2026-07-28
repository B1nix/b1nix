#!/bin/sh
# Build a standalone-bootable b1nix disk image (MBR + Limine + ext4 root),
# RPi/cloud-image style. The in-guest installer (/sbin/b1nix-install) just copies
# this onto a target disk. NO ports (no in-guest bootloader/mkfs) and NO root on
# the host: `limine bios-install` writes the boot stages straight into the image
# *file*, and mke2fs populates the partition with -d.
#
#   sh tools/images/mk-disk-image.sh [ARCH] [out.img]
#
# Excludes V8/Chromium by construction: it images $(BUILD)/root.ext4, which is the
# base rootfs (those ship as separate disks/modules, never in root.ext4).
set -eu

ARCH="${1:-x86}"
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT_DIR/build/$ARCH"
OUT="${2:-$BUILD/b1nix-disk.img}"
ROOTFS="$BUILD/rootfs"
KERNEL="$BUILD/kernel.elf"
for t in limine sfdisk mke2fs; do
  command -v "$t" >/dev/null 2>&1 || { echo "missing host tool: $t"; exit 1; }
done
[ -d "$ROOTFS" ] || { echo "rootfs missing — run: make ARCH=$ARCH root-image"; exit 1; }
[ -f "$KERNEL" ] || { echo "kernel missing: $KERNEL"; exit 1; }

# Limine's BIOS stage 2 (limine-bios.sys) has to live on the partition next to
# limine.conf; the stage-1/gap code is written by `limine bios-install` below.
LIMINE_DATADIR="${LIMINE_DATADIR:-$(limine --print-datadir 2>/dev/null || true)}"
if [ -z "$LIMINE_DATADIR" ] || [ ! -f "$LIMINE_DATADIR/limine-bios.sys" ]; then
  for d in /usr/share/limine /usr/local/share/limine /opt/homebrew/share/limine; do
    [ -f "$d/limine-bios.sys" ] && { LIMINE_DATADIR="$d"; break; }
  done
fi
[ -n "$LIMINE_DATADIR" ] && [ -f "$LIMINE_DATADIR/limine-bios.sys" ] || {
  echo "missing Limine boot files (set LIMINE_DATADIR)"; exit 1; }

PART_START=2048                       # 1 MiB offset → standard BIOS boot gap for core.img
SECTOR=512

# 1. Stage the boot files INTO the rootfs partition: Limine scans the partitions
#    of the boot drive for /boot/limine/limine.conf and loads its own stage 2
#    (limine-bios.sys) from the same directory. There is no rootfs.img module —
#    root= points at this very partition and the kernel's find_root_device
#    mounts it (kernel/main.c).
echo "staging /boot into rootfs..."
mkdir -p "$ROOTFS/boot/limine"
cp -f "$KERNEL" "$ROOTFS/boot/kernel.elf"
cp -f "$LIMINE_DATADIR/limine-bios.sys" "$ROOTFS/boot/limine/limine-bios.sys"
# root=LABEL= is device-name-agnostic: b1nix names AHCI disks sata0/sata0p1
# (not sda1), and the partition's ext4 is labeled b1nix-root by mke2fs below.
DISK_CMDLINE="${DISK_CMDLINE:-root=LABEL=b1nix-root}"
sed -e "s|@ARCH@|$ARCH|g" -e "s|@TIMEOUT@|5|g" \
    -e "s|@CMDLINE@|$DISK_CMDLINE|g" \
    "$ROOT_DIR/boot/limine/limine-disk.conf.in" > "$ROOTFS/boot/limine/limine.conf"

# 1b. Stage the core userland so the disk is a COMPLETE writable root (the shell
# + coreutils otherwise live only in the kernel's initramfs). /bin/init is a
# kernel built-in (resolves regardless of rootfs); native applets are built-ins
# too. We add busybox + zsh + the upstream applet symlinks + a minimal /etc.
case "$ARCH" in
  x86) TRIPLET=i686-b1nix ;;
  *)   TRIPLET=x86_64-b1nix ;;
esac
BUSYBOX="$(ls "$ROOT_DIR/build/$ARCH/ports/busybox/install/bin/busybox" "$ROOT_DIR/build/$ARCH/ports/busybox/build/busybox" "$ROOT_DIR/build/busybox-b1nix/$TRIPLET/busybox" 2>/dev/null | head -1)"
[ -n "$BUSYBOX" ] && [ -f "$BUSYBOX" ] || { echo "missing busybox ELF for $ARCH"; exit 1; }
echo "staging userland (busybox + zsh + applet symlinks)..."
mkdir -p "$ROOTFS/bin" "$ROOTFS/sbin" "$ROOTFS/opt/busybox/bin" "$ROOTFS/etc" "$ROOTFS/root" "$ROOTFS/home/user"
cp -f "$BUSYBOX" "$ROOTFS/opt/busybox/bin/busybox"
if [ ! -f "$ROOTFS/bin/zsh" ]; then
  ZSH_BIN="$ROOT_DIR/build/$ARCH/ports/zsh/install/bin/zsh"
  [ -f "$ZSH_BIN" ] || { echo "missing zsh — run: make ARCH=$ARCH root-image"; exit 1; }
  cp -f "$ZSH_BIN" "$ROOTFS/bin/zsh"
fi
# Core symlinks (mirror kernel/fs/initramfs.c) + every upstream applet.
for s in sh busybox getty "["; do ln -sf /opt/busybox/bin/busybox "$ROOTFS/bin/$s"; done
ln -sf /opt/busybox/bin/busybox "$ROOTFS/sbin/getty"
awk -F'=' '/^[[:space:]]*[^#]/ { gsub(/[[:space:]]/,"",$1); gsub(/[[:space:]]/,"",$2);
  if ($2=="upstream" && $1!="[") print $1 }' "$ROOT_DIR/tools/configs/applet-manifest.conf" \
  | while read -r cmd; do ln -sf /opt/busybox/bin/busybox "$ROOTFS/bin/$cmd"; done
# Minimal /etc so login + a shell console work (built-in init falls back to a shell).
[ -f "$ROOTFS/etc/passwd" ] || printf 'root:x:0:0:root:/root:/bin/zsh\nuser:x:1000:1000:b1nix user:/home/user:/bin/zsh\n' > "$ROOTFS/etc/passwd"
[ -f "$ROOTFS/etc/shells" ] || printf '/bin/sh\n/bin/zsh\n/opt/busybox/bin/busybox\n' > "$ROOTFS/etc/shells"
[ -f "$ROOTFS/etc/motd" ] || echo "b1nix (installed on disk)" > "$ROOTFS/etc/motd"

# 2. Build the ext4 partition image from the rootfs (no root; -d populates it).
PART_IMG="$BUILD/.disk-part.ext4"
PART_MB="$(du -sm "$ROOTFS" | cut -f1)"
PART_MB=$(( PART_MB + PART_MB / 2 + 64 ))      # ~50% headroom + 64 MiB slack
dd if=/dev/zero of="$PART_IMG" bs=1048576 count="$PART_MB" status=none
mke2fs -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q -L b1nix-root \
       -d "$ROOTFS" "$PART_IMG"

# 3. Assemble the whole-disk image: boot gap + partition.
PART_SECTORS=$(( PART_MB * 1048576 / SECTOR ))
TOTAL_SECTORS=$(( PART_START + PART_SECTORS + 2048 ))
dd if=/dev/zero of="$OUT" bs=$SECTOR count="$TOTAL_SECTORS" status=none
# MBR partition table: one bootable Linux (0x83) partition.
printf '%s,%s,83,*\n' "$PART_START" "$PART_SECTORS" | sfdisk -q "$OUT" >/dev/null
dd if="$PART_IMG" of="$OUT" bs=$SECTOR seek=$PART_START conv=notrunc status=none
rm -f "$PART_IMG"

# 4. Install Limine's BIOS boot stages: stage 1 goes into the MBR (preserving
#    the partition table) and the gap before the first partition holds the rest.
#    It operates on the image FILE — no loop device, no mount, no root. Stage 2
#    (limine-bios.sys) and limine.conf were staged onto the partition in step 1.
limine bios-install "$OUT" --quiet
sync

printf 'created %s (%s) — standalone-bootable (Limine/BIOS), no V8/Chromium\n' "$OUT" "$(du -sh "$OUT" | cut -f1)"
echo "test: qemu-system-x86_64 -drive file=$OUT,format=raw -m 2048 -serial stdio"
