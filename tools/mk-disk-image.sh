#!/bin/sh
# Build a standalone-bootable b1nix disk image (MBR + real GRUB + ext4 root),
# RPi/cloud-image style. The in-guest installer (/sbin/b1nix-install) just copies
# this onto a target disk. NO ports (no in-guest grub/mkfs) and NO root on the
# host: grub-mkimage + grub-bios-setup operate on the image *file*, mke2fs uses -d.
#
#   sh tools/mk-disk-image.sh [ARCH] [out.img]
#
# Excludes V8/Chromium by construction: it images $(BUILD)/root.ext4, which is the
# base rootfs (those ship as separate disks/modules, never in root.ext4).
set -eu

ARCH="${1:-x86}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT_DIR/build/$ARCH"
OUT="${2:-$BUILD/b1nix-disk.img}"
ROOTFS="$BUILD/rootfs"
KERNEL="$BUILD/kernel.elf"
GRUBDIR="/usr/lib/grub/i386-pc"

for t in grub-install losetup sfdisk mke2fs; do
  command -v "$t" >/dev/null 2>&1 || { echo "missing host tool: $t"; exit 1; }
done
# Only the loop-device steps (losetup/mount/umount/grub-install) need root; the
# rest (mke2fs -d, sfdisk on a file, dd) run as the user. SUDO is empty when
# already root. With a scoped NOPASSWD sudoers drop-in for those four commands
# (see tools/sudoers.d-b1nix-diskimage), the whole build runs unattended.
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"
[ -d "$ROOTFS" ] || { echo "rootfs missing — run: make ARCH=$ARCH root-image"; exit 1; }
[ -f "$KERNEL" ] || { echo "kernel missing: $KERNEL"; exit 1; }
[ -f "$GRUBDIR/boot.img" ] || { echo "missing $GRUBDIR (install grub i386-pc)"; exit 1; }

PART_START=2048                       # 1 MiB offset → standard BIOS boot gap for core.img
SECTOR=512

# 1. Stage the boot files INTO the rootfs partition so GRUB (prefix on the
#    partition) finds them at boot. Disk-boot grub.cfg points root= at the
#    partition; the kernel's find_root_device mounts it (kernel/main.c).
echo "staging /boot into rootfs..."
mkdir -p "$ROOTFS/boot/grub"
cp -f "$KERNEL" "$ROOTFS/boot/kernel.elf"
# grub-install installs the runtime modules; we only stage kernel + grub.cfg.
# root=LABEL= is device-name-agnostic: b1nix names AHCI disks sata0/sata0p1
# (not sda1), and the partition's ext4 is labeled b1nix-root by mke2fs below.
DISK_CMDLINE="${DISK_CMDLINE:-root=LABEL=b1nix-root}"
sed -e "s|@ARCH@|$ARCH|g" -e "s|@TIMEOUT@|5|g" \
    -e "s|@CMDLINE@|$DISK_CMDLINE|g" -e "s|@MODULE_CMD@||g" \
    "$ROOT_DIR/boot/grub/grub-disk.cfg" > "$ROOTFS/boot/grub/grub.cfg"

# 1b. Stage the core userland so the disk is a COMPLETE writable root (the shell
# + coreutils otherwise live only in the kernel's initramfs). /bin/init is a
# kernel built-in (resolves regardless of rootfs); native applets are built-ins
# too. We add busybox + bash + the upstream applet symlinks + a minimal /etc.
case "$ARCH" in
  x86) TRIPLET=i686-b1nix ;;
  *)   TRIPLET=x86_64-b1nix ;;
esac
BUSYBOX="$ROOT_DIR/build/busybox-b1nix/$TRIPLET/busybox"
BASH_BIN="$(ls "$ROOT_DIR"/build/bash-src/$TRIPLET/bash-*/bash 2>/dev/null | head -1)"
[ -f "$BUSYBOX" ] || { echo "missing busybox ELF: $BUSYBOX"; exit 1; }
[ -f "$BASH_BIN" ] || { echo "missing bash ELF (build/bash-src/$TRIPLET/bash-*/bash)"; exit 1; }
echo "staging userland (busybox + bash + applet symlinks)..."
mkdir -p "$ROOTFS/bin" "$ROOTFS/sbin" "$ROOTFS/opt/busybox/bin" "$ROOTFS/etc" "$ROOTFS/root" "$ROOTFS/home/user"
cp -f "$BUSYBOX" "$ROOTFS/opt/busybox/bin/busybox"
cp -f "$BASH_BIN" "$ROOTFS/bin/bash"
# Core symlinks (mirror kernel/fs/initramfs.c) + every upstream applet.
for s in sh busybox getty "["; do ln -sf /opt/busybox/bin/busybox "$ROOTFS/bin/$s"; done
ln -sf /opt/busybox/bin/busybox "$ROOTFS/sbin/getty"
awk -F'=' '/^[[:space:]]*[^#]/ { gsub(/[[:space:]]/,"",$1); gsub(/[[:space:]]/,"",$2);
  if ($2=="upstream" && $1!="[") print $1 }' "$ROOT_DIR/tools/applet-manifest.conf" \
  | while read -r cmd; do ln -sf /opt/busybox/bin/busybox "$ROOTFS/bin/$cmd"; done
# Minimal /etc so login + a bash console work (built-in init falls back to bash).
[ -f "$ROOTFS/etc/passwd" ] || printf 'root:x:0:0:root:/root:/bin/bash\nuser:x:1000:1000:b1nix user:/home/user:/bin/bash\n' > "$ROOTFS/etc/passwd"
[ -f "$ROOTFS/etc/shells" ] || printf '/bin/sh\n/bin/bash\n/opt/busybox/bin/busybox\n' > "$ROOTFS/etc/shells"
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

# 4. Embed real GRUB via a loop device (BIOS i386-pc): grub-install builds
#    core.img (prefix on the partition), writes boot.img to the MBR preserving
#    the partition table, embeds core.img in the boot gap, and installs modules
#    into /boot/grub. Our staged grub.cfg + kernel.elf are already on the fs.
MNT="$BUILD/.disk-mnt"
LOOP=""
cleanup() { [ -n "${MNT:-}" ] && mountpoint -q "$MNT" 2>/dev/null && $SUDO umount "$MNT"; \
            [ -n "$LOOP" ] && $SUDO losetup -d "$LOOP" 2>/dev/null; rmdir "$MNT" 2>/dev/null || true; }
trap cleanup EXIT
mkdir -p "$MNT"
LOOP="$($SUDO losetup -f --show -P "$OUT")"
# Wait for the partition node (-P) to appear.
for _ in 1 2 3 4 5; do [ -e "${LOOP}p1" ] && break; sleep 1; done
$SUDO mount "${LOOP}p1" "$MNT"
$SUDO grub-install --target=i386-pc --no-floppy --boot-directory="$MNT/boot" \
  --modules="part_msdos ext2 multiboot2 normal configfile biosdisk all_video gfxterm" \
  "$LOOP"
sync

printf 'created %s (%s) — standalone-bootable, no V8/Chromium\n' "$OUT" "$(du -sh "$OUT" | cut -f1)"
echo "test: qemu-system-x86_64 -drive file=$OUT,format=raw -m 2048 -serial stdio"
