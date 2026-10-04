#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the b1nix live medium: a hybrid ISO that boots under BIOS and UEFI into
# a live session, from the same Debian root the installed image is made of.
#
#   sh tools/image/mk-b1nix-iso.sh            # build/x86_64/b1nix-live.iso
#
# Layout of the medium:
#
#   /boot/b1nix, /boot/initrd     the kernel and its initramfs, as installed
#   /boot/limine/, /EFI/BOOT/     Limine, from the limine package in the root
#   /live/root.img                the root: btrfs, zstd, flagged as a seed
#
# The root is not unpacked at boot: the initramfs script from
# b1nix-installer-config mounts root.img read-only and sprouts it onto a zram
# device, so the session is writable and runs from compressed RAM.
#
# Like mk-b1nix-image.sh this runs as an ORDINARY USER: the tree is assembled
# in the user-namespace chroot, and everything that copies it keeps its owners
# by running in that namespace (debian-chroot.sh nsrun).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
DEB_ARCH="${DEB_ARCH:-amd64}"
SUITE="${SUITE:-trixie}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/$ARCH}"
ISO="${ISO:-$BUILD_DIR/b1nix-live.iso}"
LABEL="${LABEL:-B1NIX_LIVE}"
# Beyond boot=b1nix-live. console=ttyS0 is what a lane reads; a person booting
# the medium gets the same console on the screen.
CMDLINE_EXTRA="${CMDLINE_EXTRA:-console=ttyS0}"
REPO="${REPO:-$ROOT_DIR/build/packages/repo}"
WORK="${WORK:-$ROOT_DIR/build/images}"
ROOTFS_BASE="$WORK/rootfs-live"
ROOTFS="$ROOTFS_BASE/chroot-$SUITE-$DEB_ARCH"
STAGE="$WORK/stage-live"
ISO_TREE="$WORK/iso-live"
CHROOT="$ROOT_DIR/tools/deb/debian-chroot.sh"

SYSTEM_PKGS="$(grep -v '^#' "$ROOT_DIR/tools/image/b1nix-system-packages" | tr '\n' ' ')"
# What only the live medium carries, from Debian.
LIVE_PKGS="${LIVE_PKGS:-$(grep -v '^#' "$ROOT_DIR/tools/image/b1nix-live-packages" | tr '\n' ' ' || true)}"
# And from the overlay.
OVERLAY_PKGS="b1nix-kernel b1nix-base-files b1nix-tools b1nix-installer-config limine"

log() { printf '\033[1;34m[mk-iso]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[mk-iso] %s\033[0m\n' "$*" >&2; exit 1; }

for t in xorriso mkfs.btrfs btrfstune python3; do
	command -v "$t" >/dev/null 2>&1 || die "missing host tool: $t"
done
ls "$REPO"/dists/"$SUITE"/Release >/dev/null 2>&1 ||
	die "no repository at $REPO -- run tools/deb/build-deb.sh and tools/deb/publish-repo.sh"

in_rootfs() { CHROOT_BASE="$ROOTFS_BASE" BUILD_DEPS="$SYSTEM_PKGS $LIVE_PKGS" sh "$CHROOT" run "$@"; }
nsrun() { sh "$CHROOT" nsrun "$@"; }

_vh="$ROOT_DIR/kernel/include/b1nix/version.h"
RELEASE="$(sed -n 's/^#define B1NIX_LINUX_ABI_RELEASE[ \t]*"\([^"]*\)".*/\1/p' "$_vh" | head -1)-b1nix-$(sed -n 's/^#define B1NIX_VERSION_STR[ \t]*"\([^"]*\)".*/\1/p' "$_vh" | head -1)"

mkdir -p "$WORK" "$BUILD_DIR"

# The root image is the slow part and does not depend on the command line, so
# a medium that differs from the last one only in CMDLINE_EXTRA -- which is
# how the live and install lanes cut theirs -- reuses it. The stamp names
# every package in the repository and this script; REUSE=0 rebuilds anyway.
STAMP="$ISO_TREE/.root-stamp"
stamp_now() {
	for d in "$REPO"/pool/main/*.deb; do
		[ -f "$d" ] && printf '%s %s\n' "$(basename "$d")" "$(stat -c %Y "$d")"
	done
	printf 'self %s\n' "$(stat -c %Y "$0")"
	printf 'pkgs %s %s\n' "$SYSTEM_PKGS" "$LIVE_PKGS"
}
if [ "${REUSE:-1}" = 1 ] && [ -f "$ISO_TREE/live/root.img" ] && [ -f "$STAMP" ] &&
   [ "$(stamp_now)" = "$(cat "$STAMP")" ]; then
	log "root image is current -- reusing it (REUSE=0 to rebuild it anyway)"
	REUSE_ROOT=1
else
	REUSE_ROOT=0
fi

if [ "$REUSE_ROOT" = 0 ]; then
# ── 1. the root tree ────────────────────────────────────────────────────────
log "assembling the $SUITE live root"
CHROOT_BASE="$ROOTFS_BASE" BUILD_DEPS="$SYSTEM_PKGS $LIVE_PKGS" sh "$CHROOT" create ||
	die "could not build the root tree"

SHIPPED_SOURCE="$ROOTFS/etc/apt/sources.list.d/b1nix.sources"
[ ! -f "$SHIPPED_SOURCE" ] || mv "$SHIPPED_SOURCE" "$SHIPPED_SOURCE.build-disabled"
repo_rel="${REPO#"$ROOT_DIR"/}"
in_rootfs "printf 'deb [trusted=yes] file:/src/$repo_rel $SUITE main\n' >/etc/apt/sources.list.d/b1nix.list" ||
	die "could not add the overlay repository"
in_rootfs "apt-get update -qq && apt-get clean" || die "apt-get update failed in the live root"
in_rootfs "dpkg --configure -a" >/dev/null 2>&1 || true
in_rootfs "DEBIAN_FRONTEND=noninteractive apt-get install -y --reinstall $OVERLAY_PKGS b1nix-kernel-$RELEASE" ||
	die "installing the overlay failed"
[ ! -f "$SHIPPED_SOURCE.build-disabled" ] || mv "$SHIPPED_SOURCE.build-disabled" "$SHIPPED_SOURCE"
rm -f "$ROOTFS/etc/apt/sources.list.d/b1nix.list"
[ -f "$ROOTFS/boot/b1nix-$RELEASE" ] || die "no kernel $RELEASE in the live root"
[ -f "$ROOTFS/boot/initrd-$RELEASE" ] || die "no initramfs for $RELEASE in the live root"
grep -q b1nix-live "$ROOTFS/usr/share/initramfs-tools/scripts/b1nix-live" 2>/dev/null ||
	die "the live boot script is not installed"

# ── 2. the live session ─────────────────────────────────────────────────────
# A live medium logs straight in, as root, with no password: there is nothing
# on it to protect, and a person trying it should not need to be told one.
printf 'b1nix-live\n' >"$ROOTFS/etc/hostname"
in_rootfs "sed -i 's|^root:[^:]*:|root::|' /etc/shadow" || die "could not clear the root password"
# The root comes from the initramfs; there is nothing for fstab to mount.
: >"$ROOTFS/etc/fstab"
# The rest of the live session -- autologin, the installer launcher, the
# in-guest checks of the live and install lanes -- is b1nix-installer-config's,
# so that removing the package from an installed system removes all of it.
in_rootfs "mkinitramfs -o /boot/initrd-$RELEASE $RELEASE" ||
	die "rebuilding the initramfs with the live boot script failed"

# ── 3. the root image ───────────────────────────────────────────────────────
# Single-copy metadata: the image is a read-only seed on a medium that is
# itself the redundancy, and btrfs's default DUP metadata plus its chunk
# allocation made a 177 MB tree into a 676 MB image (125 MB this way).
log "writing the seed root image"
ROOT_IMG="$WORK/live-root.img"
nsrun "rm -rf '$STAGE' '$ROOT_IMG'" || die "could not clear the previous stage"
mkdir -p "$STAGE"
# /boot is on the medium beside the image, and nothing in the session reads it.
nsrun "tar -C '$ROOTFS' --exclude=./boot/* --exclude=./proc/* --exclude=./sys/* \
	--exclude=./src --exclude=./.b1nix-chroot-ready --exclude=./var/cache/apt/archives/*.deb \
	-cf - . | tar -C '$STAGE' --same-owner -xf - &&
	truncate -s 8G '$ROOT_IMG' &&
	mkfs.btrfs -q -L b1nix-live-root -m single -d single --compress zstd:15 \
		--rootdir '$STAGE' --shrink '$ROOT_IMG' &&
	btrfstune -S 1 '$ROOT_IMG'" || die "building the seed image failed"

rm -rf "$ISO_TREE"
mkdir -p "$ISO_TREE/boot/limine" "$ISO_TREE/EFI/BOOT" "$ISO_TREE/live"
cp "$ROOTFS/boot/b1nix-$RELEASE" "$ISO_TREE/boot/b1nix"
cp "$ROOTFS/boot/initrd-$RELEASE" "$ISO_TREE/boot/initrd"
LIM="$ROOTFS/usr/share/limine"
cp "$LIM/limine-bios.sys" "$LIM/limine-bios-cd.bin" "$LIM/limine-uefi-cd.bin" "$ISO_TREE/boot/limine/"
cp "$LIM/BOOTX64.EFI" "$ISO_TREE/EFI/BOOT/BOOTX64.EFI"
[ -s "$ROOT_IMG" ] || die "no root image at $ROOT_IMG"
mv "$ROOT_IMG" "$ISO_TREE/live/root.img"
stamp_now >"$STAMP"
fi  # REUSE_ROOT

# ── 4. the medium ───────────────────────────────────────────────────────────
log "writing the ISO"
cat >"$ISO_TREE/boot/limine/limine.conf" <<EOF
timeout: 3
serial: yes

/b1nix live
    protocol: multiboot2
    path: boot():/boot/b1nix
    cmdline: boot=b1nix-live b1nix.live-label=$LABEL $CMDLINE_EXTRA
    module_path: boot():/boot/initrd
    module_string: initrd
EOF
rm -f "$ISO"
xorriso -as mkisofs -R -r -J -V "$LABEL" \
	-b boot/limine/limine-bios-cd.bin \
	-no-emul-boot -boot-load-size 4 -boot-info-table \
	--efi-boot boot/limine/limine-uefi-cd.bin \
	-efi-boot-part --efi-boot-image --protective-msdos-label \
	"$ISO_TREE" -o "$ISO" >/dev/null 2>&1 || die "xorriso failed"
iso_rel="${ISO#"$ROOT_DIR"/}"
in_rootfs "limine bios-install /src/$iso_rel --quiet" || die "limine bios-install failed"
log "live medium at $ISO ($(du -h "$ISO" | cut -f1))"
