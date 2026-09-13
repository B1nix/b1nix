#!/bin/sh
# Stage Alpine's BusyBox into the root filesystem.
#
#   B1NIX_ARCH=x86_64 sh tools/packages/stage-busybox.sh
#
# The binary is Alpine's, pinned in alpine.lock. It lands where Alpine puts it,
# /bin/busybox, which it also re-executes itself through (BUSYBOX_EXEC_PATH).
# su, passwd and login need euid 0, so they point at a setuid copy rather than
# making the multicall binary itself setuid; FEATURE_SUID drops the privilege
# for any other applet reached through that copy. /sbin/init is BusyBox init.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"
ROOTFS="$ROOT_DIR/build/$ARCH/rootfs"
OUT="$ROOT_DIR/build/$ARCH/ports/busybox"
CIC="$ROOT_DIR/tools/copy-if-changed.sh"

rm -rf "$OUT/pkg"
mkdir -p "$OUT/pkg"
B1NIX_ARCH="$ARCH" ALPINE_LAYOUT=native \
	ALPINE_SKIP_DEPS="musl alpine-baselayout alpine-baselayout-data alpine-keys alpine-release" \
	sh "$ROOT_DIR/tools/packages/alpine-fetch.sh" "$OUT/pkg" busybox >/dev/null
[ -f "$OUT/pkg/bin/busybox" ] || { echo "stage-busybox: no bin/busybox in the package" >&2; exit 1; }

sh "$CIC" "$OUT/pkg/bin/busybox" "$OUT/busybox"
mkdir -p "$ROOTFS/bin" "$ROOTFS/sbin"
sh "$CIC" --mode 0755 "$OUT/busybox" "$ROOTFS/bin/busybox"
sh "$CIC" --mode 4755 "$OUT/busybox" "$ROOTFS/bin/busybox-suid"
[ "$(readlink "$ROOTFS/sbin/init" 2>/dev/null)" = "/bin/busybox" ] ||
	ln -sfn /bin/busybox "$ROOTFS/sbin/init"
rm -rf "$ROOTFS/opt/busybox"
