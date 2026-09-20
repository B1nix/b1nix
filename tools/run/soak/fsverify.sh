#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Filesystem writes under load, judged by tools that are not this kernel.
#
# Boots the fsverify soak workload with a fresh ext4 disk and a fresh btrfs
# disk, then:
#   - e2fsck -fn and btrfs check --check-data-csum must accept the images;
#   - every file is extracted with debugfs / btrfs restore (no mount, no b1nix
#     code in the path) and its sha256 must equal the guest's manifest;
#   - the extracted set must be exactly the manifest's set.
# A cache that loses or tears a write passes its own read-back and fails here.
#
# Usage: sh tools/run/soak/fsverify.sh [name]
# Environment: SMP (default 4), MEM_MB (default 1024, small enough that the block cache
# evicts), SOAK_SCALE, SOAK_FROZEN_DIR, OUT_DIR (default smoke_run/soak/fsv).
set -u
SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
ROOT_DIR="$(cd "$(dirname "$SELF")/../../.." && pwd)"
NAME="${1:-fsv}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/smoke_run/soak/fsv}"
SMP="${SMP:-4}"
MEM_MB="${MEM_MB:-1024}"
mkdir -p "$OUT_DIR"
W="$OUT_DIR/$NAME-smp$SMP"
rm -rf "$W"
mkdir -p "$W"

truncate -s 1G "$W/ext4.img" "$W/btrfs.img"
mkfs.ext4 -F -q -L b1nix-fsv-ext4 "$W/ext4.img" || exit 2
mkfs.btrfs -f -q -L b1nix-fsv-btrfs "$W/btrfs.img" >/dev/null || exit 2

export SMP MEM_MB OUT_DIR="$W"
export SOAK_EXTRA_ARGS="-drive file=$W/ext4.img,format=raw,if=virtio -drive file=$W/btrfs.img,format=raw,if=virtio"
TIMEOUT="${TIMEOUT:-600}" sh "$ROOT_DIR/tools/run/soak/run-soak.sh" fsverify "$NAME" > "$W/run.out" 2>&1
LOG=$(ls "$W"/soak-*.log 2>/dev/null | head -1)
bad=""
note() { bad="$bad; $*"; }

grep -q ": pass in" "$W/run.out" || note "guest verdict: $(tail -1 "$W/run.out")"
[ -n "$LOG" ] || { echo "fsverify $NAME smp=$SMP: FAIL no log"; exit 1; }

e2fsck -fn "$W/ext4.img" > "$W/e2fsck.log" 2>&1 || note "e2fsck: $(tail -3 "$W/e2fsck.log" | tr '\n' ' ')"
btrfs check --readonly --check-data-csum "$W/btrfs.img" > "$W/btrfs-check.log" 2>&1 ||
	note "btrfs check: $(tail -3 "$W/btrfs-check.log" | tr '\n' ' ')"

mkdir -p "$W/x/ext4" "$W/x/btrfs"
debugfs -R "rdump /d $W/x/ext4" "$W/ext4.img" > "$W/debugfs.log" 2>&1
btrfs restore -i "$W/btrfs.img" "$W/x/btrfs" > "$W/restore.log" 2>&1

for fs in ext4 btrfs; do
	root="$W/x/$fs/d"
	grep -a "^FSV-MAN $fs " "$LOG" | tr -d '\r' | cut -d' ' -f3- | sort > "$W/man-$fs.txt"
	want=$(grep -a "^FSV-FILES $fs " "$LOG" | tr -d '\r' | cut -d' ' -f3)
	have=$(wc -l < "$W/man-$fs.txt")
	[ -n "$want" ] && [ "$want" = "$have" ] || note "$fs manifest incomplete ($have of ${want:-?})"
	[ "$have" -gt 0 ] || note "$fs manifest empty"
	# The guest read everything back through a fresh mount; anything it could
	# not read is a checksum (or worse) failure the kernel itself saw.
	reread=$(grep -a "^FSV-REREAD $fs " "$LOG" | tr -d '\r' | tail -1)
	case "$reread" in
	*"unreadable=0") ;;
	"") note "$fs re-read line missing" ;;
	*) note "$fs guest re-read: ${reread#FSV-REREAD $fs }" ;;
	esac
	(cd "$root" 2>/dev/null && find . -type f | sed 's|^\./||' | sort | while read -r f; do
		echo "$f $(sha256sum < "$f" | cut -d' ' -f1)"
	done) > "$W/got-$fs.txt"
	awk '{print $2, $1}' "$W/man-$fs.txt" | sort > "$W/want-$fs.txt"
	if ! cmp -s "$W/want-$fs.txt" "$W/got-$fs.txt"; then
		note "$fs content differs: $(diff "$W/want-$fs.txt" "$W/got-$fs.txt" | head -4 | tr '\n' ' ')"
	fi
done

if [ -z "$bad" ]; then
	echo "fsverify $NAME smp=$SMP: PASS ext4 $(wc -l < "$W/want-ext4.txt") files, btrfs $(wc -l < "$W/want-btrfs.txt") files"
	exit 0
fi
echo "fsverify $NAME smp=$SMP: FAIL${bad}"
exit 1
