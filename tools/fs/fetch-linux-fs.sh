#!/bin/sh
# Fetch the upstream Linux filesystems and stage them for the kernel build.
#
# Same import rule as M101's DRM core, for the same reason: nothing under the
# staged tree is ever edited. A patch to imported source is a bug in the shim.
# There is deliberately no patch directory and no place to put one.
#
# What is staged and what is not:
#
#   fs/btrfs, fs/ext4, fs/jbd2   imported, built as-is
#   include/uapi/linux/*         imported — these are the on-disk and ioctl
#                                definitions, and rewriting them by hand is how
#                                a structure layout silently stops matching the
#                                disk
#   include/linux/*              NOT staged. Those are what kernel/include/linux
#                                reimplements; staging them would pull Linux's
#                                whole internal object model in behind them.
#   lib/zlib_*, lib/lzo,         imported. btrfs compresses with all three and
#   lib/zstd                     these are self-contained algorithm libraries
#                                with a narrow interface — writing our own
#                                DEFLATE would be a second implementation of a
#                                format whose output has to be byte-compatible
#                                with what every other kernel writes.
#   crypto/*                     NOT staged. The checksum algorithms reach the
#                                imported code through the shim's crypto_shash,
#                                which is a far smaller surface than Linux's
#                                crypto framework and its dependency closure.
#
# Licensing: b1nix is GPL-2.0-only (see LICENSE), and so are fs/btrfs, fs/ext4
# and fs/jbd2 — unlike the DRM core there is no MIT question to answer here, and
# no file has to be excluded on licence grounds. include/uapi carries
# GPL-2.0 WITH Linux-syscall-note. Recorded in THIRD_PARTY_NOTICES.md.
#
# The release is pinned exactly as the DRM import pins it: a version variable
# and a checksum, so the same source is fetched on every machine. Bumping
# LINUX_VERSION is a deliberate act that also requires a new SHA256.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

LINUX_VERSION="${LINUX_VERSION:-6.18.51}"
# Pinned tarballs, checked against kernel.org's signed sha256sums.asc.
case "$LINUX_VERSION" in
6.6)     LINUX_SHA256="d926a06c63dd8ac7df3f86ee1ffc2ce2a3b81a2d168484e76b5b389aba8e56d0" ;;
6.18.51) LINUX_SHA256="ba2f60f858bf4d1f929101faa356c93dc8b925b17aaa9f95eabd4627758df613" ;;
*) echo "fetch-linux-fs: no pinned SHA256 for linux-$LINUX_VERSION" >&2; exit 1 ;;
esac
TARBALL="linux-${LINUX_VERSION}.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/${TARBALL}"

# Shared with the DRM import: one tarball on disk, two staged trees out of it.
SRC_PARENT="$ROOT_DIR/build/src/linux"
STAGE_DIR="$ROOT_DIR/build/src/fs-${LINUX_VERSION}"

mkdir -p "$SRC_PARENT"

if [ -f "$STAGE_DIR/B1NIX-OBJECTS" ]; then
	echo "$STAGE_DIR"
	exit 0
fi

TAR_PATH="$SRC_PARENT/$TARBALL"
if [ ! -f "$TAR_PATH" ]; then
	echo "fetch-linux-fs: downloading $TARBALL" >&2
	curl -L "$URL" -o "$TAR_PATH.part" 1>&2
	mv "$TAR_PATH.part" "$TAR_PATH"
fi

# Verify before extracting, not after: a truncated or substituted tarball must
# never reach the tree, and "it built fine" is not a checksum.
have="$(sha256sum "$TAR_PATH" | cut -d' ' -f1)"
if [ "$have" != "$LINUX_SHA256" ]; then
	echo "fetch-linux-fs: SHA256 mismatch for $TARBALL" >&2
	echo "  expected $LINUX_SHA256" >&2
	echo "  got      $have" >&2
	echo "  (delete $TAR_PATH to re-download, or update LINUX_SHA256 if the pin moved on purpose)" >&2
	exit 1
fi

echo "fetch-linux-fs: staging fs/{btrfs,ext4,jbd2} from linux-${LINUX_VERSION}" >&2
rm -rf "$STAGE_DIR.tmp"
mkdir -p "$STAGE_DIR.tmp"

tar -xf "$TAR_PATH" -C "$STAGE_DIR.tmp" --strip-components=1 \
	"linux-${LINUX_VERSION}/fs/btrfs" \
	"linux-${LINUX_VERSION}/fs/ext4" \
	"linux-${LINUX_VERSION}/fs/jbd2"

# Four more pieces the three filesystems stand on. Each of these is a Linux
# subsystem in its own right with a narrow published interface, and each is
# imported rather than shimmed for the same reason: a reimplementation would be
# a second copy of an algorithm whose correctness the filesystem depends on.
#
#   fs/iomap      btrfs and ext4 both do direct and buffered I/O through it
#                 (with fs/internal.h, which its files include as "../internal.h")
#   fs/quota      the disk-quota subsystem ext4 accounts through; imported for
#                 the usual reason — the on-disk quota-file format (v2, a
#                 radix tree of dqblks) is a format, not an interface, and a
#                 second implementation of it would be wrong in a way only
#                 another kernel could notice
#   fs/mbcache    ext4's xattr block deduplication cache
#   lib/maple_tree ext4 uses it for its extent status tree
#   lib/xarray, lib/radix-tree, lib/idr
#                 the xarray and the two older interfaces built on it. 6.x
#                 btrfs walks its extent-buffer tree by mark with xa_state
#                 cursors, which is the data structure itself rather than an
#                 interface a shim can stand in for. The DRM core uses the same
#                 three, so they are linked for the whole kernel.
#   include/linux/{jbd2,journal-head,iomap,mbcache,maple_tree}.h — their own
#                 interfaces,
#                 which belong to the imported code, not to the shim
#
# The shim's job stops at what Linux's core provides (VFS, MM, block, locking).
# These are consumers of that core, exactly like btrfs is.
tar -xf "$TAR_PATH" -C "$STAGE_DIR.tmp" --strip-components=1 \
	"linux-${LINUX_VERSION}/fs/quota" \
	"linux-${LINUX_VERSION}/fs/iomap" \
	"linux-${LINUX_VERSION}/fs/mbcache.c" \
	"linux-${LINUX_VERSION}/lib/maple_tree.c" \
	"linux-${LINUX_VERSION}/lib/xarray.c" \
	"linux-${LINUX_VERSION}/lib/radix-tree.c" \
	"linux-${LINUX_VERSION}/lib/radix-tree.h" \
	"linux-${LINUX_VERSION}/lib/idr.c" \
	"linux-${LINUX_VERSION}/fs/internal.h" \
	"linux-${LINUX_VERSION}/lib/zlib_inflate" \
	"linux-${LINUX_VERSION}/lib/zlib_deflate" \
	"linux-${LINUX_VERSION}/lib/lzo" \
	"linux-${LINUX_VERSION}/lib/zstd" \
	"linux-${LINUX_VERSION}/lib/xxhash.c" \
	"linux-${LINUX_VERSION}/include/linux/zlib.h" \
	"linux-${LINUX_VERSION}/include/linux/zutil.h" \
	"linux-${LINUX_VERSION}/include/linux/zconf.h" \
	"linux-${LINUX_VERSION}/include/linux/lzo.h" \
	"linux-${LINUX_VERSION}/include/linux/zstd.h" \
	"linux-${LINUX_VERSION}/include/linux/zstd_errors.h" \
	"linux-${LINUX_VERSION}/include/linux/zstd_lib.h" \
	"linux-${LINUX_VERSION}/include/linux/xxhash.h" \
	"linux-${LINUX_VERSION}/include/linux/quota.h" \
	"linux-${LINUX_VERSION}/include/linux/quotaops.h" \
	"linux-${LINUX_VERSION}/include/linux/dqblk_v1.h" \
	"linux-${LINUX_VERSION}/include/linux/dqblk_v2.h" \
	"linux-${LINUX_VERSION}/include/linux/dqblk_qtree.h" \
	"linux-${LINUX_VERSION}/include/linux/jbd2.h" \
	"linux-${LINUX_VERSION}/include/linux/journal-head.h" \
	"linux-${LINUX_VERSION}/include/linux/iomap.h" \
	"linux-${LINUX_VERSION}/include/linux/mbcache.h" \
	"linux-${LINUX_VERSION}/include/linux/maple_tree.h"

# The uapi headers the three trees include. Named individually rather than
# staging include/uapi/linux wholesale: that directory is 500 files, most of
# them nothing to do with a filesystem, and a wholesale copy would shadow the
# shim's own uapi headers for every other subsystem in the build.
tar -xf "$TAR_PATH" -C "$STAGE_DIR.tmp" --strip-components=1 \
	"linux-${LINUX_VERSION}/include/uapi/linux/btrfs.h" \
	"linux-${LINUX_VERSION}/include/uapi/linux/btrfs_tree.h" \
	"linux-${LINUX_VERSION}/include/uapi/linux/fiemap.h" \
	"linux-${LINUX_VERSION}/include/uapi/linux/fsmap.h" \
	"linux-${LINUX_VERSION}/include/uapi/linux/falloc.h" \
	"linux-${LINUX_VERSION}/include/uapi/linux/magic.h" \
	"linux-${LINUX_VERSION}/include/uapi/linux/ext4.h" \
	"linux-${LINUX_VERSION}/include/uapi/linux/quota.h" \
	"linux-${LINUX_VERSION}/include/uapi/linux/dqblk_xfs.h"

# The object lists, taken from upstream's own Makefiles rather than chosen here,
# so they cannot drift from the pinned source.
#
# Only the unconditional `-y` sets. The Kconfig-gated additions (POSIX ACLs,
# fs-verity, fs-encryption, check-integrity, ref-verify) are each a subsystem
# the shim does not have yet; they are added when it does, one at a time and
# for a stated reason, rather than swept in by a wildcard.
# Paths in B1NIX-OBJECTS are relative to the staged root, not to fs/: one of
# the entries comes from lib/, and a list that silently assumed a common prefix
# would have had to special-case it at every consumer instead of once here.
emit_objs() {
	dir="$1"; shift
	for var in "$@"; do
		sed -n "/^${var}[[:space:]]*[:+]\{0,1\}=/,/[^\\\\]\$/p" "$STAGE_DIR.tmp/$dir/Makefile" |
			grep -oE '[a-z0-9_-]+\.o' | sed 's/\.o$/.c/'
	done | sort -u | while read -r f; do
		# The lists name the module object too (btrfs.o, ext4.o, jbd2.o); there is
		# no such source file, and asking make to build one is a missing-file
		# error pointing at this script rather than at the build.
		[ -f "$STAGE_DIR.tmp/$dir/$f" ] || continue
		echo "$dir/$f"
	done
}

{
	emit_objs fs/btrfs btrfs-y
	emit_objs fs/ext4 ext4-y
	emit_objs fs/jbd2 jbd2-objs
	# Quotas. The set is named rather than parsed because every entry in that
	# Makefile is Kconfig-gated and the choice IS the configuration: the
	# accounting core, the v2 on-disk format ext4 uses and the radix tree it is
	# stored in, plus kqid's id translation. quota.c is the quotactl(2) entry
	# point, which belongs to a syscall layer b1nix has its own of, and
	# netlink.c broadcasts warnings nothing here listens for.
	echo "fs/quota/dquot.c"
	echo "fs/quota/quota_tree.c"
	echo "fs/quota/quota_v2.c"
	echo "fs/quota/kqid.c"
	# iomap's unconditional set is only its tracing and its iterator; everything
	# that actually moves blocks is under CONFIG_BLOCK, which for b1nix is always
	# on — there is no other kind of filesystem here.
	emit_objs fs/iomap iomap-y 'iomap-\$(CONFIG_BLOCK)'
	# Two single files pulled out of larger directories: there is no -y list to
	# read for either, so naming them is the honest form.
	echo "fs/mbcache.c"
	echo "lib/maple_tree.c"
	echo "lib/xarray.c"
	echo "lib/radix-tree.c"
	echo "lib/idr.c"
	# The compression libraries. Every .c under them is built — unlike the
	# filesystems, these directories contain nothing that is conditional, and
	# their own Makefiles list the same set with paths this loop would have to
	# reconstruct.
	(cd "$STAGE_DIR.tmp" && find lib/zlib_inflate lib/zlib_deflate lib/lzo \
		lib/zstd -name '*.c' | sort)
	# xxhash, which btrfs offers as one of its four checksum algorithms.
	echo "lib/xxhash.c"
} > "$STAGE_DIR.tmp/B1NIX-OBJECTS"

count="$(wc -l < "$STAGE_DIR.tmp/B1NIX-OBJECTS" | tr -d ' ')"
# 109 at the 6.6 pin (58 btrfs, 33 ext4, 6 jbd2, 4 quota, 6 iomap, mbcache,
# maple_tree). The floor is a parse guard,
# not the count: a Makefile whose continuation lines stop being matched yields a
# handful of entries, not ninety.
if [ "$count" -lt 95 ]; then
	echo "fetch-linux-fs: object list has only $count entries — Makefile parse failed" >&2
	exit 1
fi

rm -rf "$STAGE_DIR"
mv "$STAGE_DIR.tmp" "$STAGE_DIR"
echo "fetch-linux-fs: staged $count objects" >&2
echo "$STAGE_DIR"
