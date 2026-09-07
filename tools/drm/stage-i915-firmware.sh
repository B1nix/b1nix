#!/bin/sh
# tools/drm/stage-i915-firmware.sh - put the i915 firmware the driver asks for
# into the image, taken from the build machine.
#
#   stage-i915-firmware.sh ROOTFS [NAME...]
#
# i915 loads its display microcontroller's firmware at probe and turns off
# runtime power management without it:
#
#   [drm] Failed to load DMC firmware i915/kbl_dmc_ver1_04.bin.
#   Disabling runtime power management.
#
# The blobs are Intel's, redistributable under the terms in linux-firmware's
# LICENSE.i915, and they are NOT committed here: this copies whatever the build
# machine already has under /lib/firmware/i915, which is where the distribution
# put them. A machine without linux-firmware simply produces an image without
# them, and the driver says so at probe as it does now — that is a missing
# package, not a broken build, so this never fails a build.
#
# .zst and .xz are handled because that is how the files are shipped now; the
# guest's loader reads a plain file out of the rootfs.
set -eu

ROOTFS="$1"; shift
[ -n "${1:-}" ] || set -- kbl_dmc_ver1_04.bin

SRC_DIR="${I915_FIRMWARE_DIR:-/lib/firmware/i915}"
DST_DIR="$ROOTFS/lib/firmware/i915"

staged=0
for name in "$@"; do
	dst="$DST_DIR/$name"
	[ -f "$dst" ] && { staged=$((staged + 1)); continue; }

	src=""
	for cand in "$SRC_DIR/$name" "$SRC_DIR/$name.zst" "$SRC_DIR/$name.xz"; do
		[ -f "$cand" ] && { src="$cand"; break; }
	done
	[ -n "$src" ] || continue

	mkdir -p "$DST_DIR"
	case "$src" in
	*.zst) command -v zstd >/dev/null 2>&1 || continue
	       zstd -dcq "$src" > "$dst.tmp" ;;
	*.xz)  command -v xz >/dev/null 2>&1 || continue
	       xz -dc "$src" > "$dst.tmp" ;;
	*)     cp -f "$src" "$dst.tmp" ;;
	esac
	mv -f "$dst.tmp" "$dst"
	staged=$((staged + 1))
done

[ "$staged" -gt 0 ] && echo "i915 firmware: $staged file(s) in $DST_DIR" >&2
exit 0
