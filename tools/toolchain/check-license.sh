#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Every source file says which licence it is under, and b1nix's own code says
# GPL-2.0-only. A file with no tag is the failure this catches: it is the state
# every file was in before, and the one a new file falls back into unless
# something asks. A file with a DIFFERENT tag is not an error -- it is
# third-party, and it must be listed below with where it came from, so that
# "which parts are not ours" is answerable by running this rather than by
# remembering.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"

OWN="GPL-2.0-only"

# Third-party files carried in-tree, and the licence each is taken under.
# Keyed by path so that moving one is a deliberate edit here too.
third_party_tag() {
	case "$1" in
	# Vendored verbatim from Linux 6.18.51 -- the tag is upstream's.
	kernel/include/b1nix/io_uring_abi.h)   echo "MIT" ;;
	kernel/include/vdso/unaligned.h)       echo "GPL-2.0" ;;
	kernel/include/linux/xarray.h)         echo "GPL-2.0+" ;;
	kernel/include/linux/maple_tree.h)     echo "GPL-2.0+" ;;
	kernel/include/linux/radix-tree.h)     echo "GPL-2.0-or-later" ;;
	kernel/include/linux/idr.h)            echo "GPL-2.0-only" ;;
	kernel/include/linux/cleanup.h|\
	kernel/include/linux/args.h|\
	kernel/include/linux/unaligned.h|\
	kernel/include/linux/unaligned/*)      echo "GPL-2.0" ;;
	# The RP2350 hardware-in-the-loop bridge: these three files are derived
	# from the pico-sdk / TinyUSB examples. The rest of that directory is ours.
	tools/board/rp2350/uart-bridge.c|\
	tools/board/rp2350/usb-descriptors.c|\
	tools/board/rp2350/tusb_config.h)      echo "MIT" ;;
	*) echo "" ;;
	esac
}

untagged=0
wrong=0
for f in $(git ls-files '*.c' '*.h' '*.S' '*.s' '*.sh' '*.py' |
           grep -v '^third_party/'); do
	tag=$(head -3 "$f" | sed -n 's#.*SPDX-License-Identifier: *\([^ */]*\).*#\1#p' | head -1)
	want=$(third_party_tag "$f")
	[ -n "$want" ] || want="$OWN"
	if [ -z "$tag" ]; then
		echo "check-license: no SPDX tag: $f"
		untagged=$((untagged + 1))
	elif [ "$tag" != "${want%% *}" ]; then
		echo "check-license: $f says '$tag', expected '$want'"
		wrong=$((wrong + 1))
	fi
done

if [ "$untagged" != 0 ] || [ "$wrong" != 0 ]; then
	echo "check-license: FAILED — $untagged untagged, $wrong mismatched"
	echo "  b1nix's own code is $OWN; third-party files are listed in this script"
	echo "  and inventoried in docs/licensing.md"
	exit 1
fi
echo "check-license: OK — every source file is tagged, own code is $OWN"
