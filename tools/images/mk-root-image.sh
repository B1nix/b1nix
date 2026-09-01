#!/bin/sh
# tools/images/mk-root-image.sh - pack the staging root into build/<arch>/root.ext4.
#
#   mk-root-image.sh ROOTFS IMAGE SIZE_MB
#
# MKE2FS/DEBUGFS name the tools; ROOT_IMAGE_FORCE=1 repacks unconditionally.
#
# Repacked only when the staged tree actually changed. Building the image is a
# fresh half-gigabyte file and a full mke2fs of the tree -- minutes, every time,
# including the many rebuilds where only the kernel moved. The test is the one
# make would apply: is any staged file newer than the image.
#
# The ownership pass belongs inside that test, and used to sit outside it. It
# writes to the image, so it gave root.ext4 a new timestamp on every build -- and
# the ISOs that carry it as a module were then repacked too, half a gigabyte
# each, for an image whose contents had not moved.
set -eu

ROOTFS="$1"; IMAGE="$2"; SIZE_MB="$3"
MKE2FS="${MKE2FS:-mke2fs}"
DEBUGFS="${DEBUGFS:-debugfs}"

# Stale by what actually goes into the image, not by any directory timestamp.
#
# The test used to be "is anything under the staging root newer than the
# image", which counts directories -- and a directory's mtime moves when an
# entry is created and removed again, even though nothing that reaches the
# image changed. With zero files newer than the image, a no-op build still
# rewrote 2.5 GB of filesystem and 2.7 GB of ISO, taking three minutes of which
# half a minute was CPU: the rest was the disk. On an SSD that is wear, paid
# for nothing, on every build.
#
# So: the set of paths (catches additions and deletions, directories included)
# plus a -newer test restricted to files and symlinks (catches edits without
# the directory false positive).
#
# This used to describe the tree with `find -printf`, which BSD find (macOS)
# does not have. The manifest came out EMPTY, and an empty manifest compares
# equal to the equally empty one already on disk -- so root.ext4 was written
# once and then reported "up to date" forever, and every aarch64 lane booted
# whatever rootfs happened to exist that first time. Hence the -s test below:
# an empty manifest is never a match.
MANIFEST="$IMAGE.manifest"
current_manifest() {
	(cd "$ROOTFS" && find . \( -type f -o -type l -o -type d \) -print) |
		LC_ALL=C sort
}
edited_since_image() {
	(cd "$ROOTFS" && find . \( -type f -o -type l \) -newer "$IMAGE" -print) |
		head -n 1
}
if [ "${ROOT_IMAGE_FORCE:-0}" != "1" ] && [ -f "$IMAGE" ] && [ -s "$MANIFEST" ] &&
   current_manifest | cmp -s - "$MANIFEST" && [ -z "$(edited_since_image)" ]; then
	printf 'up to date %s (%s)\n' "$IMAGE" "$(du -sh "$IMAGE" | cut -f1)"
	exit 0
fi

dd if=/dev/zero of="$IMAGE" bs=1048576 count="$SIZE_MB" 2>/dev/null
# The kernel's ext4 driver does not implement metadata_csum, 64bit, flex_bg or
# huge_file. An mke2fs too old to know the option names still has to produce
# something, hence the fallback.
"$MKE2FS" -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q -L b1nix-root \
	-E root_owner=0:0 -d "$ROOTFS" "$IMAGE" 2>/dev/null ||
"$MKE2FS" -t ext4 -q -L b1nix-root -E root_owner=0:0 -d "$ROOTFS" "$IMAGE"

# Everything in a Unix root filesystem belongs to root. `mke2fs -d` instead
# copies the BUILD HOST's uid/gid onto every file (501:20 on a macOS checkout),
# so the guest saw a rootfs owned by a nonexistent user. That breaks any in-guest
# ownership check: OpenPAM refuses to read a policy file it does not see as
# root-owned and pam_start() failed with PAM_SYSTEM_ERR.
#
# One batched debugfs pass over the whole tree, not one process per file.
OWN="$IMAGE.own"
(cd "$ROOTFS" && find . \( -type f -o -type d -o -type l \) -print) |
	sed -e 's|^\.||' -e '/^$/d' |
	awk '{ printf "sif \"%s\" uid 0\nsif \"%s\" gid 0\n", $0, $0 }' > "$OWN"
"$DEBUGFS" -w -f "$OWN" "$IMAGE" >/dev/null 2>&1 || true
rm -f "$OWN"

# The setuid inodes, and the one file that must not be world-readable. Shared
# with _mkimg in tests/smoke.sh, which builds the per-lane disks the aarch64
# instances boot from the same staging tree.
DEBUGFS="$DEBUGFS" sh "$(dirname "$0")/stamp-root-modes.sh" "$IMAGE"

printf 'created %s (%s)\n' "$IMAGE" "$(du -sh "$IMAGE" | cut -f1)"

current_manifest > "$MANIFEST"
