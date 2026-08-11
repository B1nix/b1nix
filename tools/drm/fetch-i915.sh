#!/bin/sh
# Stage Intel's i915 driver for M102a. Opt-in: nothing builds it by default.
#
# The DRM core is staged unconditionally because the kernel links it. i915 is
# not, and deliberately: it is 13 MiB and 264 objects, and someone working on
# the filesystem or the network stack should not pay for a GPU driver they are
# not touching. `make i915-fetch` stages it; the build picks it up if and only
# if the staged tree is there, and is byte-for-byte unchanged when it is not.
#
# Same rules as the core (tools/drm/fetch-drm-core.sh): same pinned release,
# same checksum verified before extraction, and imported source is never
# edited — a patch here would be a bug in kernel/lkpi.
#
# Licensing. i915 is permissive throughout: 609 files carry an MIT SPDX tag and
# the 155 untagged ones carry either the full MIT text or the historical X11
# permission grant. Four files are the exception — `i915_trace_points.c`,
# `display/intel_display_trace.c` and the two headers they instantiate are plain
# `GPL-2.0`, with no "or MIT" the way the DRM core's GPL-touched files had. They
# are pure ftrace plumbing, and b1nix has no ftrace, so they are not staged and
# not built: the tracepoint macros come from our own MIT headers instead, the
# same way every other kernel facility i915 stands on does. Excluding them is a
# choice of object list, not an edit to imported source.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

LINUX_VERSION="${LINUX_VERSION:-6.6}"
LINUX_SHA256="d926a06c63dd8ac7df3f86ee1ffc2ce2a3b81a2d168484e76b5b389aba8e56d0"
TARBALL="linux-${LINUX_VERSION}.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/${TARBALL}"

SRC_PARENT="$ROOT_DIR/build/src/linux"
STAGE_DIR="$ROOT_DIR/build/src/i915-${LINUX_VERSION}"

mkdir -p "$SRC_PARENT"

if [ -d "$STAGE_DIR/drivers" ]; then
	echo "$STAGE_DIR"
	exit 0
fi

TAR_PATH="$SRC_PARENT/$TARBALL"
if [ ! -f "$TAR_PATH" ]; then
	echo "fetch-i915: downloading $TARBALL" >&2
	curl -L "$URL" -o "$TAR_PATH.part" 1>&2
	mv "$TAR_PATH.part" "$TAR_PATH"
fi

# Verify before extracting, not after.
have="$(sha256sum "$TAR_PATH" | cut -d' ' -f1)"
if [ "$have" != "$LINUX_SHA256" ]; then
	echo "fetch-i915: SHA256 mismatch for $TARBALL" >&2
	echo "  expected $LINUX_SHA256" >&2
	echo "  got      $have" >&2
	echo "  (delete $TAR_PATH to re-download, or update LINUX_SHA256 if the pin moved on purpose)" >&2
	exit 1
fi

echo "fetch-i915: staging i915 from linux-${LINUX_VERSION}" >&2
rm -rf "$STAGE_DIR.tmp"
mkdir -p "$STAGE_DIR.tmp"
tar -xf "$TAR_PATH" -C "$STAGE_DIR.tmp" --strip-components=1 \
	"linux-${LINUX_VERSION}/drivers/gpu/drm/i915"

# The four GPL-2.0 tracepoint files, removed from the staged tree rather than
# merely left out of the object list: the point is that no GPL-2.0 source lands
# in the tree at all, and a header sitting there unbuilt would still be there to
# be included by accident.
rm -f "$STAGE_DIR.tmp/drivers/gpu/drm/i915/i915_trace.h" \
      "$STAGE_DIR.tmp/drivers/gpu/drm/i915/i915_trace_points.c" \
      "$STAGE_DIR.tmp/drivers/gpu/drm/i915/display/intel_display_trace.h" \
      "$STAGE_DIR.tmp/drivers/gpu/drm/i915/display/intel_display_trace.c"

# display/intel_acpi.c is GPL-2.0 too, and is the ACPI _DSM enumeration i915
# builds only under CONFIG_ACPI — it is not in i915-y and nothing here would
# build it. The VBT that Gen8/Gen9.5 actually needs comes from intel_opregion.c,
# which is MIT and stays. Removed for the same reason as the tracepoints: an
# unbuilt GPL file in the tree is still a GPL file in the tree.
rm -f "$STAGE_DIR.tmp/drivers/gpu/drm/i915/display/intel_acpi.c"

# Guard the rule rather than trusting it: anything else under the staged tree
# that is GPL without a permissive alternative is a licensing decision nobody
# made, and it should stop the build here rather than be discovered in a
# release. Dual-licensed files ("GPL-2.0 or MIT") are fine and are not matched.
strays="$(grep -rlE 'SPDX-License-Identifier:[[:space:]]*GPL-2\.0(-only)?[[:space:]]*$' \
	"$STAGE_DIR.tmp/drivers/gpu/drm/i915" --include=*.c --include=*.h 2>/dev/null |
	grep -v '/selftests\?/' | grep -v '/selftest_' || true)"
if [ -n "$strays" ]; then
	echo "fetch-i915: GPL-2.0-only files staged outside the selftests:" >&2
	echo "$strays" | sed 's|^|  |' >&2
	echo "  Decide what they are before importing them; do not edit them." >&2
	rm -rf "$STAGE_DIR.tmp"
	exit 1
fi

# The object list, from upstream's own Makefile rather than chosen here. i915-y
# is assembled from gem-y and gt-y as well, so all three are read; the line
# continuations are joined first, because an assignment spanning twenty lines is
# still one assignment and reading it line by line loses most of it.
sed -e :a -e '/\\$/N; s/\\\n//; ta' \
	"$STAGE_DIR.tmp/drivers/gpu/drm/i915/Makefile" |
	grep -E '^(i915-y|gem-y|gt-y)[[:space:]]*\+?=' |
	grep -oE '[a-zA-Z0-9_/]+\.o' |
	sed 's/\.o$/.c/' |
	sort -u > "$STAGE_DIR.tmp/B1NIX-OBJECTS.all"

# Without the tracepoint TUs, which are not staged.
grep -vxE 'i915_trace_points\.c|display/intel_display_trace\.c' \
	"$STAGE_DIR.tmp/B1NIX-OBJECTS.all" > "$STAGE_DIR.tmp/B1NIX-OBJECTS"
rm -f "$STAGE_DIR.tmp/B1NIX-OBJECTS.all"

cat > "$STAGE_DIR.tmp/B1NIX-IMPORT" <<EOF
source:  linux-${LINUX_VERSION}
sha256:  ${LINUX_SHA256}
url:     ${URL}
staged:  drivers/gpu/drm/i915 (MIT and X11-style permission grants)
removed: i915_trace.h, i915_trace_points.c, display/intel_display_trace.{c,h}
         — plain GPL-2.0, pure ftrace plumbing, replaced by MIT shim headers;
         display/intel_acpi.c — plain GPL-2.0, CONFIG_ACPI-only, not in i915-y.
rule:    imported source is never edited; fixes belong in kernel/lkpi.
opt-in:  staged by \`make i915-fetch\`; the build ignores it when absent.
EOF

mv "$STAGE_DIR.tmp" "$STAGE_DIR"
echo "$STAGE_DIR"
