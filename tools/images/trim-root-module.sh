#!/bin/sh
# Pack the staging root at a smaller size for the lanes that receive it as a
# boot module. A module is read off the emulated CD into memory before the
# kernel starts (512 MB took 33 s), and these lanes only need the files plus
# room to write.
#
# Usage: trim-root-module.sh <rootfs-dir> <src-image> <dst-image> <size-mb>
set -e

rootfs=$1
src=$2
dst=$3
size=$4

[ -f "$src" ] || { echo "trim-root-module: $src does not exist" >&2; exit 1; }
if [ -f "$dst" ] && [ ! "$src" -nt "$dst" ]; then
	exit 0
fi
# Unique names: both lane ISOs that need this build in parallel.
tmp="$dst.tmp.$$"
ROOT_IMAGE_FORCE=1 sh "$(dirname "$0")/mk-root-image.sh" "$rootfs" "$tmp" "$size" >/dev/null
rm -f "$tmp.manifest" "$tmp.fstype"
mv -f "$tmp" "$dst"
