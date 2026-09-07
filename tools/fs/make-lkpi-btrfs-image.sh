#!/bin/sh
# Build the btrfs image the imported filesystem is tested against.
#
# It is made by mkfs.btrfs, not by us: the point of the import is to read and
# write a filesystem another implementation created, and an image we produced
# would only prove we can read our own output back.
#
# The content is known and checkable from the name alone — byte N of big.bin is
# derivable from N — so the guest can verify what it read without being handed
# the answer.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT_DIR/smoke_run/lkpi-btrfs.img}"
SIZE="${LKPI_BTRFS_SIZE:-512M}"

command -v mkfs.btrfs >/dev/null 2>&1 || {
	echo "make-lkpi-btrfs-image: mkfs.btrfs not found (btrfs-progs)" >&2
	exit 1
}

STAGE="$(dirname "$OUT")/lkpi-btrfs-root-$$"
rm -rf "$STAGE"
mkdir -p "$STAGE/dir/sub"

printf 'hello from btrfs\n' > "$STAGE/hello.txt"
printf 'nested\n' > "$STAGE/dir/sub/deep.txt"
ln -s hello.txt "$STAGE/link.txt"

# Large enough to need real extents rather than an inline one, so both kinds
# are exercised. Sixteen bytes per record, so the record at byte offset N is
# N/16 written as fourteen digits and a newline.
python3 -c "
import sys
with open('$STAGE/big.bin','wb') as f:
    for i in range(192*1024//16):
        f.write(b'%014d\n' % i)
"

rm -f "$OUT"
truncate -s "$SIZE" "$OUT"
# mkfs's own defaults, free-space tree included: that is what a real disk
# carries, and the imported code has to read the filesystem as it is rather
# than one shaped to suit us.
# An attribute the HOST wrote: the guest reading it back proves the imported
# filesystem is reading the on-disk format, not something it stored itself.
if command -v setfattr >/dev/null 2>&1; then
	setfattr -n user.origin -v mkfs "$STAGE/hello.txt" 2>/dev/null || true
fi

mkfs.btrfs -q -L LKPITEST -m single -d single --nodesize 16384 \
	--rootdir "$STAGE" "$OUT"
rm -rf "$STAGE"
echo "$OUT"
