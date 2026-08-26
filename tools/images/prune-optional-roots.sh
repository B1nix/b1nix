#!/bin/sh
# tools/images/prune-optional-roots.sh - take an optional package group back out
# of the shared staging root.
#
#   prune-optional-roots.sh BUILD_DIR PKGROOT GROUP...
#
# The staging rootfs is merged into, never rebuilt, so whatever an optional
# group (browser, KDE, the GPU driver stack) copied into it once stays there for
# every ordinary image afterwards -- and 241 MB of browser is the difference
# between fitting in 512 MB and mke2fs refusing to allocate a block. Before an
# image that did not ask for a group, remove whatever belongs only to that
# group's root, derived by comparing the roots rather than by naming paths that
# would drift.
#
# Every optional group, not only the browser. Pruning one while leaving another
# in place is worse than pruning neither: the groups share libraries (icu,
# double-conversion, libdw), so removing the browser's copy of libicui18n took
# KDE's binaries' dependency out from under them, and the link gate then failed
# on an image that had asked for neither. That is not hypothetical -- it is how a
# half-finished KDE build left the tree unable to produce any image at all.
#
# A file survives if the base root has it, or if the root being built has it: the
# staging steps that follow would restore the second case anyway, but a prune
# that removes something the current image needs is a race waiting to be read as
# a missing package.
#
# Skipped when nothing that feeds it has moved. The prune is blunt on purpose --
# later steps of the image recipe put back several files it takes out, among
# them /sbin/unix_chkpwd and /etc/pam.d/other -- so running it unconditionally
# left the staging tree looking changed on every build and repacked the whole
# image behind it. The stamp records the last run; if no group's .installed and
# not the base root's is newer, there is nothing to take out.
# PRUNE_FORCE=1 runs it regardless.
set -eu

BUILD_DIR="$1"; PKGROOT="$2"; shift 2
ROOTFS="$BUILD_DIR/rootfs"
STAMP="$BUILD_DIR/.optional-roots-pruned"

[ -d "$ROOTFS" ] || exit 0

marks=""
for grp in "$@"; do
	[ -f "$BUILD_DIR/$grp/.installed" ] && marks="$marks $BUILD_DIR/$grp/.installed"
done
[ -f "$PKGROOT/.installed" ] && marks="$marks $PKGROOT/.installed"

# The stamp records WHICH package root it pruned against, not just when.
#
# Building without an optional group prunes that group out of the staging
# root; building with it again re-uses a package root whose .installed has not
# moved, so the merge that would put the files back is skipped and the image
# comes out silently missing what was asked for -- a KDE build that finished
# in 0.7 seconds and produced an image with no kwin in it. A different active
# root means the staging tree has to be merged again whatever the timestamps
# say.
if [ "${PRUNE_FORCE:-0}" != "1" ] && [ -f "$STAMP" ] &&
   [ "$(cat "$STAMP" 2>/dev/null)" = "$PKGROOT" ]; then
	newer=""
	for m in $marks; do
		[ "$m" -nt "$STAMP" ] && { newer=1; break; }
	done
	[ -n "$newer" ] || exit 0
fi
# The active root changed since the last prune: whatever it provides has to be
# merged in again, so make the merge look stale.
if [ -f "$STAMP" ] && [ "$(cat "$STAMP" 2>/dev/null)" != "$PKGROOT" ] &&
   [ -f "$PKGROOT/.installed" ]; then
	touch "$PKGROOT/.installed"
fi

for grp in "$@"; do
	[ "$BUILD_DIR/$grp" != "$PKGROOT" ] || continue
	[ -d "$BUILD_DIR/$grp" ] || continue
	(cd "$BUILD_DIR/$grp" && find . ! -type d ! -name .installed -print) \
		| while read -r f; do
			[ -e "$BUILD_DIR/pkgroot/$f" ] && continue
			[ -e "$PKGROOT/$f" ] && continue
			rm -f "$ROOTFS/$f"
		done
	(cd "$BUILD_DIR/$grp" && find . -type d -print) \
		| while read -r d; do
			[ -e "$BUILD_DIR/pkgroot/$d" ] && continue
			[ -e "$PKGROOT/$d" ] && continue
			rmdir "$ROOTFS/$d" 2>/dev/null || true
		done
done

printf %s "$PKGROOT" > "$STAMP"
