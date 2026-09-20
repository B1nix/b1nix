#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# kde-boot-time.sh — how long the KDE image takes to reach its desktop.
#
# Boots the KDE root image N times with one kernel and prints, per boot, the
# guest's own DESKTOP-UP time (seconds since the kernel started) and whether
# the kernel panicked. Nothing is rebuilt: the root image and ISO stage are
# used as they stand, so two kernels can be compared against the same
# userspace.
#
# Usage: sh tools/run/kde-boot-time.sh [runs] [kernel.elf] [extra cmdline]
#   runs          boots to make (default 3)
#   kernel.elf    kernel to boot (default build/x86_64/kernel.elf)
#   extra cmdline appended to the image's command line, e.g. "b1nix.sysprof"
#   RUN_SECONDS   how long each boot runs (default 20)
#   KEEP_LOGS=1   keep every boot's log as smoke_run/kde-boot-time-<n>.log
# A boot that panics keeps its log as smoke_run/kde-boot-time-panic-<n>.log.
set -e
DIR=$(cd "$(dirname "$0")/../.." && pwd)
RUNS=${1:-3}
KERNEL=${2:-$DIR/build/x86_64/kernel.elf}
EXTRA=${3:-}
TAG=kde-boot-time

case "$KERNEL" in /*) ;; *) KERNEL=$DIR/$KERNEL ;; esac
[ -f "$KERNEL" ] || { echo "kde-boot-time: no kernel at $KERNEL" >&2; exit 1; }
mkdir -p "$DIR/smoke_run"
# Snapshot it: a rebuild during the runs must not change what is measured.
cp "$KERNEL" "$DIR/smoke_run/$TAG-kernel.elf"

i=0
while [ "$i" -lt "$RUNS" ]; do
	i=$((i + 1))
	KDE_NO_BUILD=1 KDE_KERNEL="$DIR/smoke_run/$TAG-kernel.elf" \
		RUN_SECONDS="${RUN_SECONDS:-20}" KDE_EXTRA_CMDLINE="$EXTRA" \
		sh "$DIR/tests/kde-smoke.sh" "$TAG" > "$DIR/smoke_run/$TAG.out" 2>&1 || true
	LOG=$DIR/smoke_run/$TAG.log
	up=$(grep -a "KDE: DESKTOP-UP" "$LOG" 2>/dev/null | head -1 | sed 's/.*t=//')
	bound=$(grep -a "KDE: ok plasmashell-bound" "$LOG" 2>/dev/null | head -1 | sed 's/.*t=\([0-9.]*\).*/\1/')
	[ "${KEEP_LOGS:-0}" = 1 ] && cp "$LOG" "$DIR/smoke_run/$TAG-$i.log"
	if grep -aq "KERNEL PANIC" "$LOG" 2>/dev/null; then
		cp "$LOG" "$DIR/smoke_run/$TAG-panic-$i.log"
		echo "boot $i: PANIC $(grep -a 'KERNEL PANIC' "$LOG" | head -1 | cut -c1-100)"
	else
		echo "boot $i: desktop ${up:-none} (plasmashell bound ${bound:-none})"
	fi
done
