#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# List scripts that nothing in the tree mentions.
#
#   sh tools/toolchain/unreferenced-scripts.sh
#
# A name that appears only in its own file is a script no Makefile target, no
# lane and no document reaches. That is not proof it is dead -- someone may run
# it by hand, and docs/distro/cleanup.md says what proof looks like -- but it
# is the short list worth reading, and the list is otherwise assembled by hand
# every time somebody wonders how many scripts this tree has.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"

total=0
orphans=0
for f in tests/*.sh tests/*.py tools/*/*.sh tools/*/*.py; do
	[ -f "$f" ] || continue
	total=$((total + 1))
	name=$(basename "$f")
	hits=$(grep -rlF "$name" \
		--include=Makefile --include=*.sh --include=*.py --include=*.md \
		--include=*.c --include=*.conf --include=*.start . 2>/dev/null |
		grep -v "^\./build/" | grep -v "^\./smoke_run/" | grep -vxF "./$f" |
		wc -l | tr -d ' ')
	if [ "$hits" = "0" ]; then
		printf 'unreferenced  %s\n' "$f"
		orphans=$((orphans + 1))
	fi
done
printf '\n%d scripts, %d referenced by nothing\n' "$total" "$orphans"
