#!/bin/sh
# Produce a trimmed copy of the root image for the lanes that receive it as a
# boot module.
#
# A module is read off the emulated CD and copied into memory in full before the
# kernel starts. Measured: 512 MB of root.ext4 takes 33 s to load that way,
# while the switchroot lane's own work takes 4 s — the lane was almost entirely
# bootloader. The image is 512 MB because that is a comfortable size for a
# writable root on a disk; as a module it only has to hold the files plus room
# for what these lanes write, so the copy is shrunk to fit.
#
# Usage: trim-root-module.sh <src.ext4> <dst.ext4> <size-mb>
set -e

src=$1
dst=$2
size=$3

[ -f "$src" ] || { echo "trim-root-module: $src does not exist" >&2; exit 1; }
if [ -f "$dst" ] && [ ! "$src" -nt "$dst" ]; then
	exit 0
fi

for _t in resize2fs e2fsck; do
	command -v "$_t" >/dev/null 2>&1 || {
		# Without e2fsprogs the module is simply the full image: slower to
		# load, identical in content.
		echo "trim-root-module: $_t not found, using the untrimmed image" >&2
		cp -f "$src" "$dst"
		exit 0
	}
done

# Unique temporary: both lane ISOs that need this run in parallel under make
# -j, and a shared name meant one of them moved the file out from under the
# other. The move itself is atomic, so whichever finishes last wins and both
# see a complete image.
tmp="$dst.tmp.$$"

src_mb=$(( $(wc -c < "$src") / 1048576 ))
if [ "$src_mb" -le "$size" ]; then
	# Already no larger than asked for: nothing to gain, and resize2fs would
	# be asked to GROW it.
	cp -f "$src" "$tmp"
	mv -f "$tmp" "$dst"
	exit 0
fi

cp -f "$src" "$tmp"
# resize2fs refuses to touch a filesystem it has not seen checked, and the
# source was built by mke2fs -d, so the check is a formality that still has to
# happen. It reports 1 for "errors corrected", which is not a failure here.
e2fsck -fy "$tmp" >/dev/null 2>&1 || true
if ! resize2fs "$tmp" "${size}M" >/dev/null 2>&1; then
	echo "trim-root-module: resize2fs refused, using the untrimmed image" >&2
	mv -f "$tmp" "$dst"
	exit 0
fi
# resize2fs shrinks the filesystem inside the file; the file keeps its old
# length until it is truncated, and it is the file the bootloader reads.
truncate -s "${size}M" "$tmp"
mv -f "$tmp" "$dst"
