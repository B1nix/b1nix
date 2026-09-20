#!/bin/sh
# Pack the staging root at a smaller size for the lanes that receive it as a
# boot module. A module is read off the emulated CD into memory before the
# kernel starts (477 MB took 31 s against 3.5 s for a module-less image), and
# these lanes only need the files plus room to write.
#
# mkfs.btrfs --rootdir ignores the size of the file it is given and picks its
# own, about twice the tree. So the tree is packed with --shrink, which leaves
# no room at all, and then grown offline by exactly the room asked for.
#
# Usage: trim-root-module.sh <rootfs-dir> <src-image> <dst-image> <room-mb>
# Environment: ROOT_FS (btrfs or ext4), ROOT_MODULE_COMPRESS (btrfs only,
# e.g. zstd; empty packs the tree uncompressed).
set -e

rootfs=$1
src=$2
dst=$3
room=$4

[ -f "$src" ] || { echo "trim-root-module: $src does not exist" >&2; exit 1; }
# Rebuilt when the root changes, and when the packing asked for does.
params="${ROOT_FS:-btrfs} room=$room compress=${ROOT_MODULE_COMPRESS:-}"
if [ -f "$dst" ] && [ ! "$src" -nt "$dst" ] && [ ! "$0" -nt "$dst" ] &&
   [ "$(cat "$dst.params" 2>/dev/null)" = "$params" ]; then
	exit 0
fi
# Unique names: both lane ISOs that need this build in parallel.
tmp="$dst.tmp.$$"
mib=1048576
case "${ROOT_FS:-btrfs}" in
btrfs)
	command -v btrfs >/dev/null 2>&1 || { echo "trim-root-module: btrfs not found (btrfs-progs)" >&2; exit 1; }
	ROOT_IMAGE_FORCE=1 ROOT_BTRFS_SHRINK=1 ROOT_BTRFS_COMPRESS="${ROOT_MODULE_COMPRESS:-}" \
		sh "$(dirname "$0")/mk-root-image.sh" "$rootfs" "$tmp" "$room" >/dev/null
	packed=$(stat -c %s "$tmp")
	size=$((packed + room * mib))
	# Linux refuses a btrfs below 256 MiB unless it was made in mixed mode.
	[ "$size" -ge $((256 * mib)) ] || size=$((256 * mib))
	truncate -s "$size" "$tmp"
	btrfs filesystem resize --offline max "$tmp" >/dev/null
	;;
*)
	# mke2fs keeps the size it is given: the tree plus a tenth for metadata.
	tree=$(du -sm "$rootfs" | cut -f1)
	ROOT_IMAGE_FORCE=1 sh "$(dirname "$0")/mk-root-image.sh" "$rootfs" "$tmp" \
		$((tree + tree / 10 + room)) >/dev/null
	;;
esac
rm -f "$tmp.manifest" "$tmp.fstype"
mv -f "$tmp" "$dst"
echo "$params" > "$dst.params"
