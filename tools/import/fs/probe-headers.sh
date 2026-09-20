#!/bin/sh
# Report how far the filesystem shim is from compiling the imported source.
#
# Two numbers, and both are needed:
#
#   preprocess-clean   files whose whole include closure resolves. This is the
#                      only measure that moves monotonically while the headers
#                      are being written, because the preprocessor stops at the
#                      FIRST missing include in a file — so a histogram of
#                      missing headers under-reports by design and shrinks in
#                      jumps rather than steadily.
#   missing            the headers blocking the rest, most-blocking first.
#                      This is the work queue, not the progress bar.
#
# With --syntax it runs -fsyntax-only instead, which is the harder question:
# every declaration has to exist and match. Use that once preprocess-clean is
# the whole set.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
LINUX_VERSION="${LINUX_VERSION:-6.18.51}"
STAGE_DIR="$ROOT_DIR/build/src/fs-${LINUX_VERSION}"
GEN_DIR="$ROOT_DIR/build/src/fs-${LINUX_VERSION}-gen"
CC="${CC:-clang}"

MODE=-E
case "${1:-}" in
	--syntax) MODE=-fsyntax-only ;;
	"") ;;
	*) echo "usage: $0 [--syntax]" >&2; exit 2 ;;
esac

[ -f "$STAGE_DIR/B1NIX-OBJECTS" ] || { echo "probe-headers: run tools/import/fs/fetch-linux-fs.sh first" >&2; exit 1; }
[ -d "$GEN_DIR" ] || sh "$ROOT_DIR/tools/import/fs/gen-shim-headers.sh" >/dev/null

RES="$("$CC" -print-resource-dir)/include"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The same flags the kernel build will use for these objects. Kept here rather
# than sourced from the Makefile on purpose: this script has to run before the
# Makefile rules exist, and a drift between the two shows up as a probe that
# disagrees with the build, which is loud rather than silent.
#
# The two pointer-type diagnostics are demoted, and only for THIS code.
#
# Upstream builds these filesystems with GCC, where both are warnings, and it
# relies on that in two places: a `u64 *` handed a `loff_t *` (the same 64 bits
# with different signedness), and a struct first named inside a function-pointer
# parameter list, which becomes a type local to that prototype and then refuses
# to match the real one. Both are upstream's own code and neither can be fixed
# without editing it, which is the one thing this import forbids.
#
# b1nix's own shim files are NOT built with these off — they keep -Wall -Wextra
# and a clean bill — so a pointer bug written here is still a build failure.
set -- -std=gnu11 -nostdinc -ffreestanding -fno-builtin -fno-stack-protector \
	-fno-pic -mno-red-zone -w \
	-Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types \
	-D__KERNEL__ -D__linux__ -DKBUILD_MODNAME='"b1nixfs"' \
	-DCONFIG_X86=1 -DCONFIG_X86_64=1 \
	-isystem "$RES" \
	-I "$ROOT_DIR/kernel/include" -I "$ROOT_DIR/kernel/include/uapi" \
	-I "$GEN_DIR" \
	-I "$STAGE_DIR/include" -I "$STAGE_DIR/include/uapi" -I "$STAGE_DIR/fs" \
	-include linux/compiler_types.h -include linux/types.h \
	-include trace/events/b1nix-pasted.h -include b1nix-fwd.h

total=0
clean=0
for f in $(cat "$STAGE_DIR/B1NIX-OBJECTS"); do
	total=$((total + 1))
	# Per-file timeout. A shim header can send the preprocessor into an
	# unbounded expansion — a recursive macro, or an include cycle a guard does
	# not break — and without this the whole probe hangs on one file with no
	# indication of which.
	if timeout 60 "$CC" "$@" $MODE "$STAGE_DIR/$f" -o /dev/null 2>"$TMP/err"; then
		clean=$((clean + 1))
	else
		grep -oE "'[^']+\.h' file not found" "$TMP/err" |
			sed "s/'//g;s/ file not found//" >> "$TMP/miss"
		echo "$f" >> "$TMP/failed"
		if ! [ -s "$TMP/err" ]; then
			echo "  (timed out or produced no diagnostics: $f)" >&2
		fi
		# In syntax mode the interesting output is the errors themselves.
		# Normalised to their shape — the identifier is kept, the file and line
		# are not — so the histogram counts causes rather than occurrences.
		grep -oE "error: .*" "$TMP/err" |
			sed -E "s/error: //; s/'([^']*)'/\1/g" >> "$TMP/errs"
	fi
done

echo "preprocess-clean: $clean/$total"
[ "$MODE" = -fsyntax-only ] && echo "(mode: -fsyntax-only)"
if [ "$MODE" = -fsyntax-only ] && [ -s "$TMP/errs" ]; then
	echo
	echo "errors: $(wc -l < "$TMP/errs" | tr -d " ") total, most common:"
	sort "$TMP/errs" | uniq -c | sort -rn | head -60 | sed "s/^/  /"
fi
if [ -s "$TMP/miss" ]; then
	echo
	echo "missing headers (files blocked, most first):"
	sort "$TMP/miss" | uniq -c | sort -rn | sed 's/^/  /'
fi
