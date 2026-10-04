#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# An ISO 9660 filesystem for the iso9660 smoke (tests/programs/bin/smoke/
# isofs_smoke.c), laid out the way a real live medium is: Rock Ridge and
# Joliet both present, as xorriso writes them with -R -J.
#
# Every file holds content the test can recompute on its own, so a check
# compares against a value it derived rather than against a second copy read
# through the same driver:
#
#   hello.txt                  one known line
#   <77-character name>.txt    a name neither ISO 9660 (8.3) nor Joliet (64
#                              UTF-16 characters) can hold -- only Rock Ridge
#   MixedCase.Name             case, which plain ISO 9660 folds
#   dir/nested/deeper/leaf.txt three levels down
#   link                       a Rock Ridge symlink to the leaf
#   big.bin                    5 MiB of 32-bit words, word k = k * 2654435761
#                              (mod 2^32): larger than any single read, the
#                              page cache or one zisofs block, and every word
#                              distinct, so a misplaced block cannot pass
#   zlines.txt                 65536 numbered lines, stored zisofs-compressed
#                              (xorriso's --zisofs filter), read back inflated
#
# Built rather than committed, like the btrfs image beside it, and skipped
# when xorriso is not installed -- the test says so rather than reporting a
# pass it did not earn.
set -eu
BUILD_DIR="${1:?usage: mk-isofs-test-image.sh <build-dir>}"
IMG="$BUILD_DIR/isofs-test.img"
SEED="$BUILD_DIR/isofs-seed"
# Bumped whenever the layout above changes, so an image built by an older copy
# of this script is replaced rather than tested against new expectations.
REV=2

if [ -f "$IMG" ] && [ "$(cat "$IMG.rev" 2>/dev/null)" = "$REV" ]; then
	exit 0
fi
command -v xorriso >/dev/null 2>&1 || exit 0
command -v python3 >/dev/null 2>&1 || exit 0

rm -rf "$SEED" "$IMG" "$IMG.rev"
mkdir -p "$SEED/dir/nested/deeper"
printf 'isofs test volume\n' > "$SEED/hello.txt"
printf 'a name only Rock Ridge can hold\n' \
	> "$SEED/a-file-name-far-longer-than-iso9660-or-joliet-can-hold-rock-ridge-only.txt"
printf 'case kept\n' > "$SEED/MixedCase.Name"
printf 'three levels down\n' > "$SEED/dir/nested/deeper/leaf.txt"
ln -s dir/nested/deeper/leaf.txt "$SEED/link"
python3 - "$SEED" <<'EOF'
import struct, sys
seed = sys.argv[1]
with open(seed + "/big.bin", "wb") as f:
    words = 5 * 1024 * 1024 // 4
    f.write(b"".join(struct.pack("<I", (k * 2654435761) & 0xffffffff)
                     for k in range(words)))
with open(seed + "/zlines.txt", "w") as f:
    f.write("".join("zisofs line %08d\n" % i for i in range(65536)))
EOF

# Native xorriso commands rather than its mkisofs emulation: the zisofs filter
# is only reachable that way. Rock Ridge is on by default; Joliet is asked for.
xorriso -outdev "$IMG.tmp" -blank as_needed -volid B1NIXISO -joliet on \
	-map "$SEED" / -set_filter --zisofs /zlines.txt -- -commit \
	>/dev/null 2>&1 || {
	rm -rf "$SEED" "$IMG.tmp"
	exit 0
}
mv -f "$IMG.tmp" "$IMG"
echo "$REV" > "$IMG.rev"
rm -rf "$SEED"
