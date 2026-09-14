#!/bin/sh
# tools/images/mk-root-image.sh - pack the staging root into a filesystem image.
#
#   ROOT_FS=btrfs|ext4 mk-root-image.sh ROOTFS IMAGE SIZE_MB
#
# Default btrfs: the imported Linux btrfs (kernel/fs/lkpifs.c). The image is
# labeled b1nix-root either way, and the kernel mounts whatever it probes. The
# tree is packed inside `unshare -r`, so the building user's files land owned by
# uid 0. ROOT_IMAGE_FORCE=1 repacks unconditionally.
set -eu

ROOTFS="$1"; IMAGE="$2"; SIZE_MB="$3"

# Absolute, because the staleness test runs `find` from inside $ROOTFS.
case "$IMAGE" in
	/*) ;;
	*) IMAGE="$(pwd)/$IMAGE" ;;
esac

# Stale by what goes into the image: the set of paths (additions, deletions)
# plus files and symlinks newer than it. Directory mtimes move on a create and
# remove that leaves nothing changed, so they are not compared. An empty
# manifest never matches.
MANIFEST="$IMAGE.manifest"
FSTAG="$IMAGE.fstype"
current_manifest() {
	(cd "$ROOTFS" && find . \( -type f -o -type l -o -type d \) -print) |
		LC_ALL=C sort
}
edited_since_image() {
	(cd "$ROOTFS" && find . \( -type f -o -type l \) -newer "$IMAGE" -print) |
		head -n 1
}
# A different filesystem type is a different image, whatever the tree.
[ "$(cat "$FSTAG" 2>/dev/null)" = "${ROOT_FS:-btrfs}" ] || rm -f "$MANIFEST"
if [ "${ROOT_IMAGE_FORCE:-0}" != "1" ] && [ -f "$IMAGE" ] && [ -s "$MANIFEST" ] &&
   current_manifest | cmp -s - "$MANIFEST" && [ -z "$(edited_since_image)" ]; then
	printf 'up to date %s (%s)\n' "$IMAGE" "$(du -sh "$IMAGE" | cut -f1)"
	exit 0
fi

sh "$(dirname "$0")/stamp-root-modes.sh" "$ROOTFS"
TMP="$IMAGE.tmp.$$"
rm -f "$TMP"
truncate -s "${SIZE_MB}M" "$TMP"
case "${ROOT_FS:-btrfs}" in
btrfs)
	command -v mkfs.btrfs >/dev/null 2>&1 || { echo "mk-root-image: mkfs.btrfs not found (btrfs-progs)" >&2; exit 1; }
	# Single profiles: DUP metadata would make mkfs grow the file past SIZE_MB.
	unshare -r mkfs.btrfs -q -f -L b1nix-root -m single -d single \
		--rootdir "$ROOTFS" "$TMP"
	;;
ext4)
	# Features the native ext4 driver reads; ext4-lkpi reads these too.
	unshare -r mke2fs -q -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file \
		-L b1nix-root -d "$ROOTFS" "$TMP"
	;;
*)
	echo "mk-root-image: unknown ROOT_FS=$ROOT_FS" >&2; exit 1 ;;
esac
mv -f "$TMP" "$IMAGE"
current_manifest > "$MANIFEST"
echo "${ROOT_FS:-btrfs}" > "$FSTAG"
printf 'created %s (%s)\n' "$IMAGE" "$(du -sh "$IMAGE" | cut -f1)"
