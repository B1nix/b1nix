#!/bin/sh
# tools/image/mk-root-image.sh - pack the staging root into a filesystem image.
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
	# ROOT_BTRFS_COMPRESS=zstd packs the tree compressed (the imported btrfs
	# reads zstd) and ROOT_BTRFS_SHRINK=1 drops the free space: a root loaded
	# whole into RAM by the boot loader has to fit below 4 GiB on small machines.
	unshare -r mkfs.btrfs -q -f -L b1nix-root -m single -d single \
		${ROOT_BTRFS_COMPRESS:+--compress "$ROOT_BTRFS_COMPRESS"} \
		${ROOT_BTRFS_SHRINK:+--shrink} \
		--rootdir "$ROOTFS" "$TMP"
	;;
ext4)
	# Features the native ext4 driver reads; ext4-lkpi reads these too.
	if command -v unshare >/dev/null 2>&1; then
		unshare -r mke2fs -q -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file \
			-L b1nix-root -d "$ROOTFS" "$TMP"
	else
		# No user namespaces (macOS): pack as the building user, then give
		# every inode to root with debugfs. Homebrew keeps e2fsprogs keg-only,
		# and a bare mke2fs on PATH may be Android platform-tools' copy.
		E2=""
		for d in /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin /sbin /usr/sbin; do
			[ -x "$d/debugfs" ] && [ -x "$d/mke2fs" ] && { E2="$d/"; break; }
		done
		[ -n "$E2" ] || { echo "mk-root-image: debugfs not found (e2fsprogs)" >&2; exit 1; }
		"${E2}mke2fs" -q -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file \
			-L b1nix-root -E root_owner=0:0 -d "$ROOTFS" "$TMP"
		(cd "$ROOTFS" && find . \( -type f -o -type d -o -type l \) -print) |
			sed -e 's|^\.||' -e '/^$/d' |
			awk '{ printf "sif \"%s\" uid 0\nsif \"%s\" gid 0\n", $0, $0 }' > "$TMP.own"
		"${E2}debugfs" -w -f "$TMP.own" "$TMP" >/dev/null 2>&1 ||
			{ rm -f "$TMP.own"; echo "mk-root-image: ownership pass failed" >&2; exit 1; }
		rm -f "$TMP.own"
	fi
	;;
*)
	echo "mk-root-image: unknown ROOT_FS=$ROOT_FS" >&2; exit 1 ;;
esac
mv -f "$TMP" "$IMAGE"
current_manifest > "$MANIFEST"
echo "${ROOT_FS:-btrfs}" > "$FSTAG"
printf 'created %s (%s)\n' "$IMAGE" "$(du -sh "$IMAGE" | cut -f1)"
