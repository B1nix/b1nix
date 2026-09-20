#!/bin/sh
# A minimal ISO 9660 filesystem, for the module tests that need one.
#
# The property those tests check is that a mounted filesystem pins the module
# providing it, so they need a filesystem that COMES from a module. That used
# to be btrfs; btrfs is built into the kernel now that the imported
# implementation replaced the hand-written driver, and isofs is the module
# still there to ask.
#
# Built rather than committed, like the btrfs image beside it, and skipped
# quietly when no ISO tool is installed — the test says so rather than
# reporting a pass it did not earn.
set -eu
BUILD_DIR="${1:?usage: mk-isofs-test-image.sh <build-dir>}"
IMG="$BUILD_DIR/isofs-test.img"
SEED="$BUILD_DIR/isofs-seed"

[ -f "$IMG" ] && exit 0

tool=""
for t in xorrisofs genisoimage mkisofs; do
	command -v "$t" >/dev/null 2>&1 && { tool="$t"; break; }
done
if [ -z "$tool" ] && command -v xorriso >/dev/null 2>&1; then
	tool="xorriso -as mkisofs"
fi
[ -n "$tool" ] || exit 0

rm -rf "$SEED"
mkdir -p "$SEED"
printf 'isofs test volume\n' > "$SEED/hello.txt"

# -V names the volume; the rest is the default ISO 9660 layout, which is what
# the kernel's isofs reads.
$tool -quiet -V B1NIXISO -o "$IMG.tmp" "$SEED" >/dev/null 2>&1 || {
	rm -rf "$SEED" "$IMG.tmp"
	exit 0
}
mv -f "$IMG.tmp" "$IMG"
rm -rf "$SEED"
