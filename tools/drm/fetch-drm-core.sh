#!/bin/sh
# Fetch the upstream DRM core and stage it for the kernel build.
#
# This is the import M101 is built around: the DRM core is compiled exactly as
# upstream wrote it, and everything it stands on is b1nix's own MIT linuxkpi.
# The rule that keeps that affordable is absolute — a patch to anything under
# the staged tree is a bug in the shim, not a fix here. There is deliberately no
# patch directory and no place to put one.
#
# The release is pinned the way tools/ports/* pin theirs: a version variable and
# a checksum, so the same source is fetched on every machine and a silently
# different upstream cannot creep in. Bumping LINUX_VERSION is a deliberate act
# that also requires a new SHA256.
#
# Licensing: drivers/gpu/drm and include/drm are largely MIT (the X11/DRI
# heritage), and include/uapi/drm is GPL-2.0 WITH Linux-syscall-note, whose
# exception explicitly permits non-GPL use. Nothing under include/linux is
# staged — those are GPL-2.0 without exception, and are exactly what the shim
# reimplements from scratch. See LICENSING.md and THIRD_PARTY_NOTICES.md.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

LINUX_VERSION="${LINUX_VERSION:-6.6}"
LINUX_SHA256="d926a06c63dd8ac7df3f86ee1ffc2ce2a3b81a2d168484e76b5b389aba8e56d0"
TARBALL="linux-${LINUX_VERSION}.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/${TARBALL}"

SRC_PARENT="$ROOT_DIR/build/src/linux"
STAGE_DIR="$ROOT_DIR/build/src/drm-core-${LINUX_VERSION}"

mkdir -p "$SRC_PARENT"

if [ -d "$STAGE_DIR/drivers" ] && [ -d "$STAGE_DIR/include/drm" ]; then
	echo "$STAGE_DIR"
	exit 0
fi

TAR_PATH="$SRC_PARENT/$TARBALL"
if [ ! -f "$TAR_PATH" ]; then
	echo "fetch-drm-core: downloading $TARBALL" >&2
	curl -L "$URL" -o "$TAR_PATH.part" 1>&2
	mv "$TAR_PATH.part" "$TAR_PATH"
fi

# Verify before extracting, not after: a truncated or substituted tarball must
# never reach the tree, and "it built fine" is not a checksum.
have="$(sha256sum "$TAR_PATH" | cut -d' ' -f1)"
if [ "$have" != "$LINUX_SHA256" ]; then
	echo "fetch-drm-core: SHA256 mismatch for $TARBALL" >&2
	echo "  expected $LINUX_SHA256" >&2
	echo "  got      $have" >&2
	echo "  (delete $TAR_PATH to re-download, or update LINUX_SHA256 if the pin moved on purpose)" >&2
	exit 1
fi

# Only what the core needs. The vendor drivers (i915, amdgpu, nouveau) are
# M102's business and are staged by their own milestones; pulling all 477 MiB of
# drivers/gpu/drm here would import code nothing builds yet.
echo "fetch-drm-core: staging DRM core from linux-${LINUX_VERSION}" >&2
rm -rf "$STAGE_DIR.tmp"
mkdir -p "$STAGE_DIR.tmp"
tar -xf "$TAR_PATH" -C "$STAGE_DIR.tmp" --strip-components=1 \
	"linux-${LINUX_VERSION}/include/drm" \
	"linux-${LINUX_VERSION}/include/uapi/drm"

# The core's own .c/.h files, without descending into the per-vendor subdirs.
# --no-wildcards-match-slash is load-bearing: without it GNU tar lets `*` cross
# directory separators, so "drivers/gpu/drm/*.c" quietly matches
# drivers/gpu/drm/i915/*.c as well and stages 478 MiB of vendor drivers that
# nothing builds.
mkdir -p "$STAGE_DIR.tmp/drivers/gpu/drm"
tar -xf "$TAR_PATH" -C "$STAGE_DIR.tmp" --strip-components=1 \
	--wildcards --no-wildcards-match-slash \
	"linux-${LINUX_VERSION}/drivers/gpu/drm/*.c" \
	"linux-${LINUX_VERSION}/drivers/gpu/drm/*.h"

# Two more pieces the core needs that are MIT in their own right and therefore
# imported rather than reimplemented: the HDMI infoframe library (linux/hdmi.h +
# drivers/video/hdmi.c, "Permission is hereby granted, free of charge...") and
# video/nomodeset.h (SPDX MIT). Writing our own versions of these would be
# rewriting working MIT code for no reason — the rule is import what is
# importable, and shim only what is not.
tar -xf "$TAR_PATH" -C "$STAGE_DIR.tmp" --strip-components=1 \
	"linux-${LINUX_VERSION}/include/linux/hdmi.h" \
	"linux-${LINUX_VERSION}/include/video/nomodeset.h" \
	"linux-${LINUX_VERSION}/drivers/video/hdmi.c" \
	"linux-${LINUX_VERSION}/drivers/video/nomodeset.c"

# The object list, taken from upstream's own Makefile rather than chosen here.
# Not every file in drivers/gpu/drm is meant to be built — Kconfig selects them,
# and drm_of.c for instance is device-tree-only and collides with its own
# header's stub when built without it. `drm-y` is the set that is always built,
# so it is the set we build, and it comes from the pinned source so it cannot
# drift from it.
# The assignment continues while lines end in a backslash; it does NOT end at
# the first blank line, and stopping there swallows the drm-$(CONFIG_*) blocks
# that follow — which is how drm_of.c, built only with CONFIG_OF, ended up in a
# list of files that are always built.
tar -xOf "$TAR_PATH" "linux-${LINUX_VERSION}/drivers/gpu/drm/Makefile" |
	awk '/^drm-y[[:space:]]*:=/ { inblock = 1 }
	     inblock { print; if ($0 !~ /\\$/) exit }' |
	grep -oE 'drm_[a-z0-9_]+\.o' |
	sed 's/\.o$/.c/' |
	sort -u > "$STAGE_DIR.tmp/B1NIX-OBJECTS"

# drm_kms_helper-y as well. "The DRM core" in practice means drm.ko plus
# drm_kms_helper.ko: the atomic modeset helpers, the probe helpers and the
# rectangle maths live there, and every vendor driver builds on them. Splitting
# them out is a module boundary, and b1nix links the whole thing into the kernel
# — so the boundary buys nothing here and leaving the helpers out would only
# mean discovering they were needed later.
tar -xOf "$TAR_PATH" "linux-${LINUX_VERSION}/drivers/gpu/drm/Makefile" |
	awk '/^drm_kms_helper-y[[:space:]]*:=/ { inblock = 1 }
	     inblock { print; if ($0 !~ /\\$/) exit }' |
	grep -oE 'drm_[a-z0-9_]+\.o' |
	sed 's/\.o$/.c/' >> "$STAGE_DIR.tmp/B1NIX-OBJECTS"

# hdmi.c is not in either list — upstream builds it alongside the video helpers —
# but the core links against its infoframe helpers, so it is part of what has to
# be built here.
echo "hdmi.c" >> "$STAGE_DIR.tmp/B1NIX-OBJECTS"

sort -u -o "$STAGE_DIR.tmp/B1NIX-OBJECTS" "$STAGE_DIR.tmp/B1NIX-OBJECTS"

# Record what this tree is, next to it, so a stray copy can still be identified.
cat > "$STAGE_DIR.tmp/B1NIX-IMPORT" <<EOF
source: linux-${LINUX_VERSION}
sha256: ${LINUX_SHA256}
url:    ${URL}
staged: drivers/gpu/drm/*.[ch], include/drm, include/uapi/drm,
        include/linux/hdmi.h, include/video/nomodeset.h,
        drivers/video/{hdmi,nomodeset}.c  (all MIT)
rule:   imported source is never edited; fixes belong in kernel/lkpi.
EOF

mv "$STAGE_DIR.tmp" "$STAGE_DIR"
echo "$STAGE_DIR"
