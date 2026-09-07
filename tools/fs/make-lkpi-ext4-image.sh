#!/bin/sh
# Build the ext4 image the imported filesystem is tested against.
#
# Made by mke2fs, with the features b1nix's own ext4 driver cannot read left
# ON: the point of importing Linux's ext4 is to read what Linux writes, and an
# image trimmed to suit us would prove nothing. The content matches the btrfs
# image's, so one self-test can check either.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT_DIR/smoke_run/lkpi-ext4.img}"
SIZE="${LKPI_EXT4_SIZE:-256M}"

command -v mke2fs >/dev/null 2>&1 || {
	echo "make-lkpi-ext4-image: mke2fs not found (e2fsprogs)" >&2
	exit 1
}

STAGE="$(dirname "$OUT")/lkpi-ext4-root-$$"
rm -rf "$STAGE"

# mke2fs writes the quota inodes but not an entry for every id it just gave a
# file to, so the image it produces is already inconsistent by its own fsck's
# reckoning. Let that fsck settle it here, once, rather than have the guest
# blamed for it later.
e2fsck -fy "$OUT" >/dev/null 2>&1 || true
mkdir -p "$STAGE/dir/sub"

printf 'hello from btrfs\n' > "$STAGE/hello.txt"
printf 'nested\n' > "$STAGE/dir/sub/deep.txt"
ln -s hello.txt "$STAGE/link.txt"
python3 -c "
with open('$STAGE/big.bin','wb') as f:
    for i in range(192*1024//16):
        f.write(b'%014d\n' % i)
"

# An attribute the HOST wrote, so the guest reading it proves the imported ext4
# is reading the on-disk format rather than something it stored itself.
if command -v setfattr >/dev/null 2>&1; then
	setfattr -n user.origin -v mkfs "$STAGE/hello.txt" 2>/dev/null || true
fi

rm -f "$OUT"
truncate -s "$SIZE" "$OUT"
# The quota feature is on because the imported quota core is in the build and
# only a filesystem that carries quota inodes exercises it. mke2fs writes the
# hidden inodes (3 and 4) and e2fsck checks the accounting in them, which is
# the host verdict the guest's writes are measured against.
mke2fs -q -t ext4 -O quota -L LKPIEXT4 -d "$STAGE" "$OUT" >/dev/null
rm -rf "$STAGE"

# mke2fs writes the quota inodes but no entry for the ids it just gave files
# to, so the image it produces is already inconsistent by its own fsck's
# reckoning ("Missing quota entry ID 1000"). Settle it here, once, rather than
# have the guest blamed for it afterwards.
e2fsck -fy "$OUT" >/dev/null 2>&1 || true

echo "$OUT"
