#!/bin/sh
# Build the image an imported filesystem is tested against.
#
#   make-lkpi-image.sh btrfs|ext4 [out.img]
#
# Made by the host's mkfs, not by us: the point of the import is to read and
# write what another implementation created. ext4 keeps the features b1nix's
# own driver cannot read, quota included. The content is known and checkable
# from the name alone -- byte N of big.bin is derivable from N -- and one
# self-test checks either filesystem.
set -eu

FS="${1:?usage: make-lkpi-image.sh btrfs|ext4 [out.img]}"
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${2:-$ROOT_DIR/smoke_run/lkpi-$FS.img}"

case "$FS" in
btrfs) SIZE="${LKPI_BTRFS_SIZE:-512M}"; MKFS=mkfs.btrfs ;;
ext4)  SIZE="${LKPI_EXT4_SIZE:-256M}";  MKFS=mke2fs ;;
*) echo "make-lkpi-image: unknown filesystem $FS" >&2; exit 1 ;;
esac
command -v "$MKFS" >/dev/null 2>&1 || { echo "make-lkpi-image: $MKFS not found" >&2; exit 1; }

STAGE="$(dirname "$OUT")/lkpi-$FS-root-$$"
rm -rf "$STAGE"
mkdir -p "$STAGE/dir/sub"
printf 'hello from btrfs\n' > "$STAGE/hello.txt"
printf 'nested\n' > "$STAGE/dir/sub/deep.txt"
ln -s hello.txt "$STAGE/link.txt"
# Large enough to need real extents. Sixteen bytes per record: the record at
# byte N is N/16 as fourteen digits and a newline.
python3 -c "
with open('$STAGE/big.bin','wb') as f:
    for i in range(192*1024//16):
        f.write(b'%014d\n' % i)
"
# An attribute the HOST wrote: reading it back proves the on-disk format is read.
command -v setfattr >/dev/null 2>&1 &&
	setfattr -n user.origin -v mkfs "$STAGE/hello.txt" 2>/dev/null || true

rm -f "$OUT"
truncate -s "$SIZE" "$OUT"
case "$FS" in
btrfs)
	mkfs.btrfs -q -L LKPITEST -m single -d single --nodesize 16384 --rootdir "$STAGE" "$OUT" ;;
ext4)
	mke2fs -q -t ext4 -O quota -L LKPIEXT4 -d "$STAGE" "$OUT" >/dev/null
	# mke2fs leaves quota entries missing for the ids it just assigned; let
	# its own fsck settle that rather than blame the guest later.
	e2fsck -fy "$OUT" >/dev/null 2>&1 || true ;;
esac
rm -rf "$STAGE"
echo "$OUT"
