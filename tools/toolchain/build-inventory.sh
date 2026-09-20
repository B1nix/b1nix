#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# What is in the build tree, where each part came from, and what nothing names.
#
#   sh tools/toolchain/build-inventory.sh            # report
#   sh tools/toolchain/build-inventory.sh --prune    # delete the orphans
#
# The build tree accumulates. Every investigation leaves an image behind, every
# lane that was renamed leaves its old ISO, and after a year the directory is
# forty gigabytes of which nobody can say which part is still needed. The
# question that actually matters is not "how big" but "who asks for this", and
# that is answerable: a lane or the Makefile names the artifacts it needs, and
# an artifact no name reaches is dead.
#
# It also answers the other question this tree raises -- what is ours and what
# came from somewhere else -- because the two are cleaned differently. Deleting
# something we build costs a rebuild; deleting something fetched costs a
# download, and on a slow link that is the difference between a coffee and an
# afternoon.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"
BUILD="${BUILD_DIR:-build}"
ARCH="${ARCH:-x86_64}"
PRUNE=0
[ "${1:-}" != "--prune" ] || PRUNE=1

[ -d "$BUILD" ] || { echo "no $BUILD directory" >&2; exit 1; }

# -L: build/ is a symlink to a volume with room for it, and du on the link
# itself reports four kilobytes.
human() { du -shL "$1" 2>/dev/null | cut -f1; }

# ── where each part comes from ──────────────────────────────────────────────
# FETCHED: downloaded from somewhere else and verified against a lock or a
# digest. Expensive to replace, cheap to keep.
# OURS:    produced by this tree from its own sources. Always reproducible.
printf '\n%s\n' "FETCHED — downloaded, replaceable only over the network"
for p in "$BUILD/src" "$BUILD/$ARCH/pkg" "$BUILD/$ARCH/pkgcache" \
         "$BUILD/$ARCH/ports" "$BUILD/$ARCH/debian" "$BUILD/packages/cache-trixie-amd64"; do
	[ -e "$p" ] || continue
	printf '  %-44s %6s  %s\n' "$p" "$(human "$p")" "$(
		case "$p" in
		*/src) echo "imported Linux source (DRM, filesystems)" ;;
		*/pkg|*/pkgcache) echo "Alpine packages, pinned in alpine.lock" ;;
		*/ports) echo "musl and the libraries built from fetched source" ;;
		*/debian) echo "the Debian rootfs layer from the registry" ;;
		*) echo "the Debian chroot's base layer" ;;
		esac)"
done

printf '\n%s\n' "OURS — produced here, reproducible by rebuilding"
for p in "$BUILD/$ARCH/kernel" "$BUILD/$ARCH/programs" "$BUILD/$ARCH/rootfs" \
         "$BUILD/$ARCH/modules" "$BUILD/$ARCH/inc" "$BUILD/$ARCH/vdso" \
         "$BUILD/$ARCH/toolchain" "$BUILD/selfhost-out" "$BUILD/dist"; do
	[ -e "$p" ] || continue
	printf '  %-44s %6s\n' "$p" "$(human "$p")"
done

# ── artifacts, and whether anything still names them ────────────────────────
# A lane, the Makefile or a run script names every artifact that is still in
# use. The name is matched without its directory, because that is how the
# sources spell it.
printf '\n%s\n' "ARTIFACTS — an image or ISO, and what still asks for it"
orphans=""
orphan_bytes=0
for f in "$BUILD/$ARCH"/*.iso "$BUILD/$ARCH"/*.img "$BUILD/$ARCH"/*.ext4; do
	[ -f "$f" ] || continue
	name=$(basename "$f")
	stem=${name%.iso}; stem=${stem%.img}; stem=${stem%.ext4}
	if git grep -qI -F "$name" -- Makefile tests tools 2>/dev/null ||
	   git grep -qI -F "$stem" -- Makefile tests tools 2>/dev/null; then
		printf '  %-44s %6s  named\n' "$name" "$(human "$f")"
	else
		printf '  %-44s %6s  ORPHAN\n' "$name" "$(human "$f")"
		orphans="$orphans $f"
		orphan_bytes=$((orphan_bytes + $(stat -c %s "$f")))
	fi
done

# Staging trees follow their artifact: iso-kde-panel belongs to
# b1nix-kde-panel.iso, and when the ISO is an orphan so is the tree.
for d in "$BUILD/$ARCH"/iso-* "$BUILD/$ARCH"/*-iso "$BUILD/$ARCH"/pkgroot-*; do
	[ -d "$d" ] || continue
	name=$(basename "$d")
	if git grep -qI -F "$name" -- Makefile tests tools 2>/dev/null; then
		continue
	fi
	printf '  %-44s %6s  ORPHAN (staging)\n' "$name" "$(human "$d")"
	orphans="$orphans $d"
	orphan_bytes=$((orphan_bytes + $(du -sb "$d" | cut -f1)))
done

printf '\ntotal build tree: %s, of which orphaned: %s MiB\n' \
	"$(human "$BUILD")" "$((orphan_bytes / 1048576))"

if [ -z "$orphans" ]; then
	printf 'nothing to prune\n'
	exit 0
fi
if [ "$PRUNE" = "0" ]; then
	printf 'run with --prune to delete the orphans\n'
	exit 0
fi

# Deleting build output is safe by definition -- it is rebuildable -- but an
# orphan is deleted because no NAME reaches it, and a name can live somewhere
# this script does not look. So it says what it removes.
for o in $orphans; do
	printf 'removing %s\n' "$o"
	rm -rf "$o"
done
printf 'reclaimed about %s MiB\n' "$((orphan_bytes / 1048576))"
