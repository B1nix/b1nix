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

# Absolute, because the staleness test below runs `find` from INSIDE $ROOTFS.
# With the relative path make passes ("build/x86_64/root.ext4") that find said
# "No such file or directory", printed nothing, and the empty output read as
# "nothing was edited" -- so an image whose files had all changed was reported
# up to date, and the guest booted the previous build's modules.
case "$IMAGE" in
	/*) ;;
	*) IMAGE="$(pwd)/$IMAGE" ;;
esac
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

# A few files changed: write them into the image that exists, and keep it.
#
# A one-line edit to a script in the overlay used to cost the whole image: a
# fresh 2.5 GB file, mke2fs over 50 000 files, minutes, every time. debugfs can
# write, replace and remove files inside an ext4 image in seconds. So when the
# difference between the staged tree and the last manifest is small -- edited
# files, a few added or removed paths -- it is applied in place. Every file
# written is read back and compared byte for byte, and any disagreement, any
# debugfs error, or anything this does not handle (a removed directory, a
# device node) falls through to the full repack below, which is always right.
if [ "${ROOT_IMAGE_FORCE:-0}" != "1" ] && [ -f "$IMAGE" ] && [ -s "$MANIFEST" ]; then
	NEWM="$IMAGE.manifest.new"
	current_manifest > "$NEWM"
	GONE="$IMAGE.gone"
	CAME="$IMAGE.came"
	CMDS="$IMAGE.debugfs"
	: > "$CMDS"
	comm -23 "$MANIFEST" "$NEWM" > "$GONE"
	comm -13 "$MANIFEST" "$NEWM" > "$CAME"
	EDITED="$IMAGE.edited"
	(cd "$ROOTFS" && find . \( -type f -o -type l \) -newer "$IMAGE" -print) |
		LC_ALL=C sort > "$EDITED"
	n=$(( $(wc -l < "$GONE") + $(wc -l < "$CAME") + $(wc -l < "$EDITED") ))
	ok=1
	if [ "$n" -gt 200 ]; then
		echo "mk-root-image: $n paths differ; repacking" >&2
		ok=0
	fi
	# Removed directories and special files are the full repack's business.
	if [ "$ok" = 1 ] && [ -s "$GONE" ]; then
		while IFS= read -r g; do
			case "$g" in */) ok=0 ;; esac
		done < "$GONE"
		# The manifest carries no type; a removed path that was a directory
		# is one whose name is a prefix of another removed entry, or one the
		# old image says is a directory. Ask the image.
		while IFS= read -r g && [ "$ok" = 1 ]; do
			p="${g#.}"
			if "$DEBUGFS" -R "stat \"$p\"" "$IMAGE" 2>/dev/null | grep -q "Type: *directory"; then
				ok=0
			fi
		done < "$GONE"
	fi
	if [ "$ok" = 1 ]; then
		# Order: remove what is gone, create new directories, write files.
		while IFS= read -r g; do
			printf 'rm "%s"\n' "${g#.}" >> "$CMDS"
		done < "$GONE"
		while IFS= read -r c; do
			if [ -d "$ROOTFS/$c" ] && [ ! -L "$ROOTFS/$c" ]; then
				printf 'mkdir "%s"\n' "${c#.}" >> "$CMDS"
			fi
		done < "$CAME"
		# Files and links: the edited ones and the new ones, once each.
		LC_ALL=C sort -u "$EDITED" "$CAME" | while IFS= read -r f; do
			[ -d "$ROOTFS/$f" ] && [ ! -L "$ROOTFS/$f" ] && continue
			p="${f#.}"
			if [ -L "$ROOTFS/$f" ]; then
				printf 'rm "%s"\nsymlink "%s" "%s"\n' "$p" "$p" "$(readlink "$ROOTFS/$f")" >> "$CMDS"
			elif [ -f "$ROOTFS/$f" ]; then
				mode=$(stat -c %a "$ROOTFS/$f" 2>/dev/null || stat -f %Lp "$ROOTFS/$f")
				printf 'rm "%s"\nwrite "%s" "%s"\nsif "%s" mode 0100%s\nsif "%s" uid 0\nsif "%s" gid 0\n' \
					"$p" "$ROOTFS/$f" "$p" "$p" "$mode" "$p" "$p" >> "$CMDS"
			else
				echo "mk-root-image: cannot place $f in place; repacking" >&2
				echo 'quit' > "$CMDS"; echo "REPACK" >> "$CMDS"
				break
			fi
		done
		if grep -q '^REPACK$' "$CMDS" 2>/dev/null; then
			ok=0
		fi
	fi
	if [ "$ok" = 1 ] && [ -s "$CMDS" ]; then
		"$DEBUGFS" -w -f "$CMDS" "$IMAGE" > "$CMDS.log" 2>&1 || ok=0
		if [ "$ok" = 1 ]; then
			CHK="$IMAGE.chk"
			LC_ALL=C sort -u "$EDITED" "$CAME" | while IFS= read -r f; do
				[ -f "$ROOTFS/$f" ] && [ ! -L "$ROOTFS/$f" ] || continue
				p="${f#.}"
				"$DEBUGFS" -R "dump \"$p\" $CHK" "$IMAGE" > /dev/null 2>&1
				if ! cmp -s "$CHK" "$ROOTFS/$f"; then
					echo "mk-root-image: $p did not read back; repacking" >&2
					echo FAIL > "$CHK.fail"
					break
				fi
			done
			[ -e "$CHK.fail" ] && ok=0
			rm -f "$CHK" "$CHK.fail"
		fi
		if [ "$ok" = 1 ]; then
			DEBUGFS="$DEBUGFS" sh "$(dirname "$0")/stamp-root-modes.sh" "$IMAGE"
			touch "$IMAGE"
			mv "$NEWM" "$MANIFEST"
			printf 'updated %s in place (%s paths)\n' "$IMAGE" "$n"
			rm -f "$GONE" "$CAME" "$EDITED" "$CMDS" "$CMDS.log"
			exit 0
		fi
		echo "mk-root-image: in-place update failed, repacking" >&2
	fi
	rm -f "$NEWM" "$GONE" "$CAME" "$EDITED" "$CMDS" "$CMDS.log" 2>/dev/null
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
