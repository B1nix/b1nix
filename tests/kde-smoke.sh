#!/bin/sh
# kde-smoke.sh — Plasma on the real DRM path, judged from outside the guest.
#
# The KDE image is not part of the main suite: it is a second image, an order
# of magnitude larger, and it exists to run a compositor that is not wlroots.
# This script is what decides whether that compositor actually drew, and it is
# written so that it cannot pass without one.
#
# Three things are asserted, and each is a thing the guest cannot forge:
#
#   1. The frame.  QEMU's `screendump` reads the framebuffer the guest
#      programmed into the virtual GPU; no process inside the guest takes part
#      in producing the file.  A display that never drew dumps a solid
#      rectangle -- two or three distinct colours.  A desktop with a wallpaper
#      and a panel on it has thousands.  The threshold is on that count.
#
#   2. The device open.  The kernel's own trace records every open under
#      /dev/dri with the name of the task that made it.  A compositor that
#      drew opened a card; a harness that printed a marker did not.
#
#   3. No compositor underneath.  A desktop nested in sway proves that KDE's
#      clients work, not that KDE drives a display.  The run must have taken
#      the drm backend, and no wlroots compositor may be running.
#
# Usage: sh tests/kde-smoke.sh [tag]
#   KDE_NO_BUILD=1     use build/x86_64/b1nix.iso as it stands
#   KDE_MIN_COLOURS=N  frame threshold (default 1000)
set -u

DIR=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:-kde-smoke}
LOG=$DIR/smoke_run/$TAG.log
FRAMES=$DIR/smoke_run/$TAG-frames
MIN_COLOURS=${KDE_MIN_COLOURS:-1000}

pass=0
fail=0
ok()   { pass=$((pass + 1)); echo "KDE-SMOKE: ok $1"; }
bad()  { fail=$((fail + 1)); echo "KDE-SMOKE: FAIL $1"; }

if [ "${KDE_NO_BUILD:-0}" != "1" ]; then
	echo "KDE-SMOKE: building KDE image"
	( cd "$DIR" && B1NIX_KDE=1 \
		KERNEL_CMDLINE="b1nix.kde b1nix.trace-sysfs" \
		make -j"${JOBS:-6}" iso ) || {
		echo "KDE-SMOKE: FAIL build"
		exit 1
	}
fi

mkdir -p "$DIR/smoke_run"
flock "$DIR/smoke_run/.qemu.lock" sh "$DIR/tools/run-kde.sh" "$TAG" || true

if [ ! -f "$LOG" ]; then
	echo "KDE-SMOKE: FAIL no log at $LOG"
	exit 1
fi

# 1. The compositor ran on DRM, with nothing under it.
if grep -aq "KDE: backend drm" "$LOG"; then
	ok "backend-drm"
else
	bad "backend-drm (the run did not take the real DRM path)"
fi
if grep -aqE "KDE: nested under sway|KDE: (ok|fail) nested-" "$LOG"; then
	bad "no-nested-compositor (a wlroots compositor was underneath)"
else
	ok "no-nested-compositor"
fi

# 2. udev catalogued the card, which is what gives it a seat.
#
# logind hands a device to a session only when the udev database says the
# device belongs to that session's seat. No database entry, no seat, and the
# request is refused before any open. The entry is written by udevd running
# elogind's own seat rules, so its presence is evidence the whole chain ran.
if grep -aq "KDE: ok udev-tagged-card" "$LOG"; then
	ok "udev-tagged-card"
else
	bad "udev-tagged-card (no /run/udev/data entry for the DRM card)"
	grep -a "KDE: udev db after trigger\|KDE: fail udev-tagged-card\|KDE: udevd said\|KDE: fail no-udevd" "$LOG" | head -4
fi

# 3. The compositor, or logind on its behalf, opened a card.
#
# b1nix.trace-sysfs prints every /dev/dri open with the task that made it, from
# inside the kernel.  Probes from the boot script open cards too, so the task
# name is what matters: kwin or the login daemon, not the shell.
if grep -aiE "(sysfs-open|drm): (open )?/dev/dri/card" "$LOG" \
	| grep -aiqE "kwin|elogind|logind|plasma"; then
	ok "compositor-opened-card"
	grep -aiE "(sysfs-open|drm): (open )?/dev/dri/card" "$LOG" \
		| grep -aiE "kwin|elogind|logind|plasma" | head -3
else
	bad "compositor-opened-card (no kwin/logind open of /dev/dri in the kernel trace)"
	echo "KDE-SMOKE: --- every /dev/dri open the kernel saw ---"
	grep -aiE "(sysfs-open|drm): (open )?/dev/dri" "$LOG" | head -12
fi

# 4. The picture has contents.
best=0
bestf=
if [ -d "$FRAMES" ]; then
	for f in "$FRAMES"/frame-*.ppm; do
		[ -f "$f" ] || continue
		line=$(python3 "$DIR/tools/ppm-colours.py" "$f" 2>/dev/null) || continue
		n=$(echo "$line" | sed -n 's/.*unique=\([0-9]*\).*/\1/p')
		[ -n "$n" ] || continue
		if [ "$n" -gt "$best" ]; then best=$n; bestf=$f; fi
	done
fi
echo "KDE-SMOKE: richest frame: ${bestf:-none} unique=$best (threshold $MIN_COLOURS)"
if [ "$best" -ge "$MIN_COLOURS" ]; then
	ok "desktop-drawn"
	png=$DIR/smoke_run/$TAG-shot.png
	if command -v convert > /dev/null 2>&1 && [ -n "$bestf" ]; then
		convert "$bestf" "$png" 2>/dev/null && echo "KDE-SMOKE: picture: $png"
	fi
else
	bad "desktop-drawn (the scanout never carried a desktop)"
fi

echo "KDE-SMOKE: $pass passed, $fail failed"
echo "KDE-SMOKE: log: $LOG"
[ "$fail" -eq 0 ] || exit 1
echo "KDE-SMOKE: done"
