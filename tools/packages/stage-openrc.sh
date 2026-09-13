#!/bin/sh
# Stage Alpine's OpenRC into the root filesystem, with the b1nix test-image
# configuration from tools/configs/openrc on top.
#
#   B1NIX_ARCH=x86_64 sh tools/packages/stage-openrc.sh
#
# /sbin/init is BusyBox init, which hands the runlevels to OpenRC through
# /etc/inittab, as on Alpine (whose OpenRC is built without openrc-init).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"
ROOTFS="$ROOT_DIR/build/$ARCH/rootfs"
STAGE="$ROOT_DIR/build/$ARCH/ports/openrc/pkg"
CONF="$ROOT_DIR/tools/configs/openrc"
CIC="$ROOT_DIR/tools/build/copy-if-changed.sh"

rm -rf "$STAGE"
mkdir -p "$STAGE"
B1NIX_ARCH="$ARCH" ALPINE_LAYOUT=native \
	ALPINE_SKIP_DEPS="musl alpine-baselayout alpine-baselayout-data alpine-keys alpine-release busybox busybox-binsh busybox-ifupdown ifupdown-ng" \
	sh "$ROOT_DIR/tools/packages/alpine-fetch.sh" "$STAGE" openrc >/dev/null
[ -x "$STAGE/sbin/openrc" ] || { echo "stage-openrc: no sbin/openrc in the package" >&2; exit 1; }
rm -f "$STAGE/.dummy"

# The package's own tree (links kept as links), then ours over it.
(cd "$STAGE" && find . \( -type f -o -type l \) -print0 | xargs -0 sh "$CIC" --into "$ROOTFS")
sh "$CIC" "$CONF/rc.conf" "$ROOTFS/etc/rc.conf"
rm -f "$ROOTFS/etc/openrc-ctltest.sh" "$ROOTFS/etc/local.d/zz-ctltest.start"

# Runlevels exactly as listed: links that are already right are left alone, so
# an unchanged configuration does not make the root image look stale.
want="$(grep -v '^#' "$CONF/runlevels" | awk 'NF == 2 { print $1 "/" $2 }')"
for l in "$ROOTFS"/etc/runlevels/*/*; do
	[ -L "$l" ] || continue
	rel="${l#$ROOTFS/etc/runlevels/}"
	printf '%s\n' "$want" | grep -qx "$rel" || rm -f "$l"
done
printf '%s\n' "$want" | while read -r rel; do
	[ -n "$rel" ] || continue
	mkdir -p "$ROOTFS/etc/runlevels/${rel%/*}"
	[ "$(readlink "$ROOTFS/etc/runlevels/$rel" 2>/dev/null)" = "/etc/init.d/${rel#*/}" ] ||
		ln -sfn "/etc/init.d/${rel#*/}" "$ROOTFS/etc/runlevels/$rel"
done

# Left behind by the from-source port, which installed under /libexec/openrc.
rm -rf "$ROOTFS/libexec/openrc" "$ROOTFS/sbin/openrc-init" "$ROOTFS/sbin/openrc-shutdown"
